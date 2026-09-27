// WaveformWidget.cpp
// Digital-only mode (Phase 1/2).
// Analog code is preserved under #if ANALOG_ENABLED blocks  --  set to 1 to restore.

#define ANALOG_ENABLED 0

#include "waveform/WaveformWidget.h"
#include "session/CaptureSession.h"

#if ANALOG_ENABLED
#include "analysis/Measurements.h"
#endif

#include <QMouseEvent>
#include <QMenu>
#include <QWheelEvent>
#include <QKeyEvent>
#include <QPainter>
#include <QPen>
#include <cmath>
#include <algorithm>

// --- GLSL -------------------------------------------------------------------

static const char* VERT = R"glsl(
#version 330 core
layout(location=0) in vec2 pos;
uniform mat4 mvp;
void main(){ gl_Position = mvp*vec4(pos,0,1); }
)glsl";
static const char* FRAG = R"glsl(
#version 330 core
uniform vec4 color;
out vec4 fc;
void main(){ fc=color; }
)glsl";

// --- Layout constants -------------------------------------------------------
// Digital-only: 16 channels fill the full widget height.
// When ANALOG_ENABLED=1, the top 70% is analog and bottom 30% is digital.

static constexpr int   DIG_CHANNELS = 8;   // max digital channels shown

// Helper: count how many channels are currently visible
static int visibleCount(const bool vis[8]) {
    int n = 0;
    for (int i = 0; i < DIG_CHANNELS; i++) if (vis[i]) n++;
    return n ? n : 1;
}
// Helper: map channel index to its visible row (0-based)
static int visibleRow(const bool vis[8], int ch) {
    int row = 0;
    for (int i = 0; i < ch; i++) if (vis[i]) row++;
    return row;
}
static constexpr float DIG_TOP_FRAC = 0.0f; // digital area starts at top (digital-only)
static constexpr float DIG_H_FRAC   = 1.0f; // digital area = full height

// --- Construction -----------------------------------------------------------

WaveformWidget::WaveformWidget(QWidget* p)
    : QOpenGLWidget(p)
{
    setMinimumHeight(300);
    setFocusPolicy(Qt::StrongFocus);
#if ANALOG_ENABLED
    snap_[0].reserve(4*1024*1024);
    snap_[1].reserve(4*1024*1024);
#endif
    dig_snap_.reserve(64*1024);
    wave_verts_.reserve(MAX_WAVE_VERTS*2);
    cursors_.reserve(MAX_CURSORS);
}

WaveformWidget::~WaveformWidget() {
    makeCurrent();
    grid_vbo_.destroy(); wave_vbo_.destroy(); vao_.destroy();
    doneCurrent();
}

void WaveformWidget::setSession(const escope::CaptureSession* s) { session_ = s; }

// --- Layout helpers ---------------------------------------------------------

#if ANALOG_ENABLED
WaveformWidget::ChannelLayout WaveformWidget::analogLayout(std::size_t ch) const {
    float H = height(), analog_h = H * 0.70f;
    float ch_h = analog_h / escope::CaptureSession::MAX_ANALOG_CH;
    float top  = ch * ch_h, bot = top + ch_h, mid = top + ch_h * 0.5f;
    float px_per_volt = (ch_h / VDIVS_PER_CH) / volt_per_div_[ch];
    return {top, bot, mid, px_per_volt};
}
#else
// Stub  --  required by header but never called in digital-only mode
WaveformWidget::ChannelLayout WaveformWidget::analogLayout(std::size_t) const {
    return {0, 0, 0, 1};
}
#endif

float WaveformWidget::digitalRowY(std::size_t ch, bool hi) const {
    float H       = height();
    float digi_top = H * DIG_TOP_FRAC;
    float digi_h   = H * DIG_H_FRAC;
    float row_h    = digi_h / DIG_CHANNELS;
    float top      = digi_top + ch * row_h;
    // Signal occupies 70% of row height, centred
    float margin   = row_h * 0.15f;
    return hi ? top + margin : top + row_h - margin;
}

int WaveformWidget::channelAtY(float) const {
    // Digital-only: no per-channel V/div, always return -1 (no analog channel)
    return -1;
}

int WaveformWidget::cursorHitTest(float mx, float my) const {
    constexpr float SNAP = 8.f;
    int best = -1; float best_d = SNAP + 1;
    for (int i = 0; i < (int)cursors_.size(); ++i) {
        const auto& c = cursors_[i];
        if (!c.active) continue;
        float d;
        if (c.type == WaveformCursor::Type::Vertical) {
            d = std::abs(mx - timeToPixel(c.t_ns));
        } else {
#if ANALOG_ENABLED
            d = std::abs(my - voltToPixel(c.v_volts, std::clamp(c.channel,0,1)));
#else
            d = SNAP + 1; // horizontal cursors disabled in digital-only mode
#endif
        }
        if (d < best_d) { best_d = d; best = i; }
    }
    return best;
}

// --- OpenGL lifecycle -------------------------------------------------------

void WaveformWidget::initializeGL() {
    initializeOpenGLFunctions();
    glClearColor(0.06f, 0.06f, 0.08f, 1.f);
    shader_.addShaderFromSourceCode(QOpenGLShader::Vertex,   VERT);
    shader_.addShaderFromSourceCode(QOpenGLShader::Fragment,  FRAG);
    shader_.link();
    vao_.create(); vao_.bind();
    grid_vbo_.create(); grid_vbo_.setUsagePattern(QOpenGLBuffer::StaticDraw);
    wave_vbo_.create(); wave_vbo_.setUsagePattern(QOpenGLBuffer::StreamDraw);
    wave_vbo_.bind();
    wave_vbo_.allocate(MAX_WAVE_VERTS * 2 * (int)sizeof(float));
    wave_vbo_.release();
    glEnableVertexAttribArray(0);
    vao_.release();
    buildGridVBO();
}

void WaveformWidget::resizeGL(int,int) { buildGridVBO(); }

void WaveformWidget::buildGridVBO() {
    if (!grid_vbo_.isCreated()) return;
    float W = width(), H = height();
    std::vector<float> lines;
    lines.reserve(512);

    // Vertical time-division lines
    for (int i = 0; i <= HDIVS; ++i) {
        float x = i * W / HDIVS;
        lines.insert(lines.end(), {x, 0, x, H});
    }

#if ANALOG_ENABLED
    // Analog area grid (top 70%)
    float analog_h = H * 0.70f;
    float ch_h = analog_h / escope::CaptureSession::MAX_ANALOG_CH;
    for (std::size_t ch = 0; ch < escope::CaptureSession::MAX_ANALOG_CH; ++ch)
        for (int d = 0; d <= VDIVS_PER_CH; ++d) {
            float y = ch * ch_h + d * ch_h / VDIVS_PER_CH;
            lines.insert(lines.end(), {0, y, W, y});
        }
    // Separator
    lines.insert(lines.end(), {0, H*0.70f, W, H*0.70f});
#endif

    // Digital channel row separators -- only for visible channels
    float digi_top = H * DIG_TOP_FRAC;
    float digi_h   = H * DIG_H_FRAC;
    int   n_vis    = visibleCount(ch_visible_);
    float row_h    = digi_h / n_vis;
    for (int i = 0; i <= n_vis; ++i) {
        float y = digi_top + i * row_h;
        lines.insert(lines.end(), {0, y, W, y});
    }

    grid_vertex_count_ = lines.size() / 2;
    grid_vbo_.bind();
    grid_vbo_.allocate(lines.data(), lines.size() * sizeof(float));
    grid_vbo_.release();
}

// --- Paint ------------------------------------------------------------------

void WaveformWidget::paintGL() {
    glClear(GL_COLOR_BUFFER_BIT);
    if (!session_) return;

    // Rolling follow  --  use digital buffer time range (no analog in digital-only)
    if (follow_latest_) {
        /* Anchor the right edge of the view to the newest timestamp actually
         * received, not to real wall-clock "now". Bursts only get decoded
         * (and their edges timestamped/pushed) once fully received, and that
         * can lag noticeably behind wall-clock time depending on the link.
         * Anchoring to wall-clock "now" left a growing empty gap on the
         * right of the screen for however far behind the latest burst was;
         * anchoring to the data's own latest timestamp means the view is
         * always fully populated up to whatever has actually arrived. */
        auto [t0, t1] = session_->digital_buffer().time_range_ns();
        time_offset_ns_ = std::max(0.0, t1 - time_per_div_ns_ * HDIVS);
    }

    drawGrid();

#if ANALOG_ENABLED
    drawZeroLines();
    for (std::size_t ch = 0; ch < escope::CaptureSession::MAX_ANALOG_CH; ++ch) {
        if (session_->analog_info(ch).enabled) {
            session_->analog_buffer(ch).snapshot(snap_[ch]);
            drawAnalogChannel(ch);
        }
    }
    // Update vertical cursor voltages from analog snapshots
    for (auto& c : cursors_)
        if (c.active && c.type == WaveformCursor::Type::Vertical)
            updateCursorVoltages(c);
#endif

    // Digital  --  all channels
    // Only the edges inside (or bordering) the visible window are ever drawn
    // (see drawDigitalChannel), and the initial level at the window's left
    // edge is already resolved via level_at(). Snapshotting the whole
    // capture history here made every frame's cost grow with total capture
    // length, which stalls the UI once a continuous capture accumulates a
    // large number of edges (e.g. dense per-sample bursts).
    double view_start = time_offset_ns_;
    double view_end   = time_offset_ns_ + time_per_div_ns_ * HDIVS;
    dig_snap_ = session_->digital_buffer().edges_in_range(view_start, view_end);
    int n_dig = std::min((int)escope::CaptureSession::MAX_DIGITAL_CH, DIG_CHANNELS);
    for (int ch = 0; ch < n_dig; ++ch)
        if (session_->digital_info(ch).enabled)
            drawDigitalChannel(ch);

    drawOverlay();
}

// --- Grid -------------------------------------------------------------------

void WaveformWidget::drawGrid() {
    float W = width(), H = height();
    shader_.bind();
    QMatrix4x4 mvp; mvp.ortho(0, W, H, 0, -1, 1);
    shader_.setUniformValue("mvp", mvp);
    shader_.setUniformValue("color", QVector4D(0.15f, 0.15f, 0.18f, 1.f));
    vao_.bind(); grid_vbo_.bind();
    glVertexAttribPointer(0, 2, GL_FLOAT, GL_FALSE, 0, nullptr);
    glDrawArrays(GL_LINES, 0, grid_vertex_count_);
    grid_vbo_.release(); vao_.release(); shader_.release();
}

#if ANALOG_ENABLED
void WaveformWidget::drawZeroLines() {
    if (!session_) return;
    float W = width(), H = height();
    std::vector<float> lines;
    for (std::size_t ch = 0; ch < escope::CaptureSession::MAX_ANALOG_CH; ++ch) {
        if (!session_->analog_info(ch).enabled) continue;
        auto lay = analogLayout(ch);
        float y = lay.mid - v_offset_[ch] * lay.px_per_volt;
        lines.insert(lines.end(), {0, y, W, y});
    }
    if (lines.empty()) return;
    shader_.bind();
    QMatrix4x4 mvp; mvp.ortho(0, W, H, 0, -1, 1);
    shader_.setUniformValue("mvp", mvp);
    shader_.setUniformValue("color", QVector4D(0.30f, 0.30f, 0.30f, 1.f));
    vao_.bind(); wave_vbo_.bind();
    wave_vbo_.write(0, lines.data(), lines.size() * sizeof(float));
    glVertexAttribPointer(0, 2, GL_FLOAT, GL_FALSE, 0, nullptr);
    glDrawArrays(GL_LINES, 0, lines.size() / 2);
    wave_vbo_.release(); vao_.release(); shader_.release();
}

void WaveformWidget::drawAnalogChannel(std::size_t ch) {
    const auto& samples = snap_[ch];
    if (samples.size() < 2) return;
    int W = width(), H = height();
    double vs = time_offset_ns_, ve = vs + time_per_div_ns_*HDIVS, vd = ve-vs;
    auto lay = analogLayout(ch);
    float mid = lay.mid, ppv = lay.px_per_volt, voff = v_offset_[ch];
    auto it_lo = std::lower_bound(samples.begin(), samples.end(), vs,
        [](const escope::AnalogSample& s, double t){return s.timestamp_ns<t;});
    if (it_lo != samples.begin()) --it_lo;
    auto it_hi = std::upper_bound(it_lo, samples.end(), ve,
        [](double t, const escope::AnalogSample& s){return t<s.timestamp_ns;});
    if (it_hi != samples.end()) ++it_hi;
    wave_verts_.clear();
    auto it = it_lo;
    for (int px = 0; px < W; ++px) {
        double t0 = vs+(double)px/W*vd, t1 = vs+(double)(px+1)/W*vd;
        while (it != it_hi && it->timestamp_ns < t0) ++it;
        if (it == it_hi) break;
        float vmin=1e9f, vmax=-1e9f;
        for (auto it2=it; it2!=it_hi && it2->timestamp_ns<t1; ++it2) {
            if (it2->voltage<vmin) vmin=it2->voltage;
            if (it2->voltage>vmax) vmax=it2->voltage;
        }
        if (vmin > 9e8f) continue;
        float yhi = std::clamp(mid-(vmax-voff)*ppv, lay.top, lay.bot);
        float ylo = std::clamp(mid-(vmin-voff)*ppv, lay.top, lay.bot);
        wave_verts_.insert(wave_verts_.end(), {(float)px,yhi,(float)px,ylo});
    }
    if (wave_verts_.empty()) return;
    uint32_t rgba = session_->analog_info(ch).color;
    float r=((rgba>>24)&0xFF)/255.f, g=((rgba>>16)&0xFF)/255.f, b=((rgba>>8)&0xFF)/255.f;
    shader_.bind();
    QMatrix4x4 mvp; mvp.ortho(0,W,H,0,-1,1);
    shader_.setUniformValue("mvp",mvp);
    shader_.setUniformValue("color",QVector4D(r,g,b,1.f));
    vao_.bind(); wave_vbo_.bind();
    int bytes = std::min((int)(wave_verts_.size()*sizeof(float)),MAX_WAVE_VERTS*2*(int)sizeof(float));
    wave_vbo_.write(0,wave_verts_.data(),bytes);
    glVertexAttribPointer(0,2,GL_FLOAT,GL_FALSE,0,nullptr);
    glLineWidth(1.5f);
    glDrawArrays(GL_LINES, 0, std::min((int)wave_verts_.size()/2, MAX_WAVE_VERTS));
    wave_vbo_.release(); vao_.release(); shader_.release();
}
#endif // ANALOG_ENABLED

// --- Digital channel --------------------------------------------------------

void WaveformWidget::drawDigitalChannel(std::size_t ch) {
    int W = width(), H = height();
    double vs = time_offset_ns_, ve = vs + time_per_div_ns_ * HDIVS;
    float hi_y = digitalRowY(ch, true);
    float lo_y = digitalRowY(ch, false);

    wave_verts_.clear();
    float xp = 0, yp = session_->digital_buffer().level_at(ch, vs) ? hi_y : lo_y;

    for (const auto& e : dig_snap_) {
        if (e.channel != (uint8_t)ch) continue;
        if (e.timestamp_ns < vs) { yp = e.rising ? hi_y : lo_y; xp = 0; continue; }
        if (e.timestamp_ns > ve) break;
        float x = timeToPixel(e.timestamp_ns), yn = e.rising ? hi_y : lo_y;
        wave_verts_.insert(wave_verts_.end(), {xp,yp,x,yp, x,yp,x,yn});
        xp = x; yp = yn;
    }
    wave_verts_.insert(wave_verts_.end(), {xp, yp, (float)W, yp});
    if (wave_verts_.empty()) return;

    // Colour: alternate between two greens for readability
    float g = (ch % 2 == 0) ? 0.90f : 0.65f;
    float b = (ch % 2 == 0) ? 0.15f : 0.45f;

    shader_.bind();
    QMatrix4x4 mvp; mvp.ortho(0, W, H, 0, -1, 1);
    shader_.setUniformValue("mvp", mvp);
    shader_.setUniformValue("color", QVector4D(0.10f, g, b, 1.f));
    vao_.bind(); wave_vbo_.bind();
    int bytes = std::min((int)(wave_verts_.size()*sizeof(float)), MAX_WAVE_VERTS*2*(int)sizeof(float));
    wave_vbo_.write(0, wave_verts_.data(), bytes);
    glVertexAttribPointer(0, 2, GL_FLOAT, GL_FALSE, 0, nullptr);
    glLineWidth(1.5f);
    glDrawArrays(GL_LINES, 0, std::min((int)wave_verts_.size()/2, MAX_WAVE_VERTS));
    wave_vbo_.release(); vao_.release(); shader_.release();
}

// --- Overlay (QPainter) -----------------------------------------------------

void WaveformWidget::drawOverlay() {
    QPainter p(this);
    p.setRenderHint(QPainter::Antialiasing, false);
    int W = width(), H = height();

    // Time/div label (bottom centre, above digital area)
    auto fmt_tdiv = [&]() -> QString {
        double ns = time_per_div_ns_;
        if (ns >= 1e9) return QString::number(ns/1e9,'f',2) + " s/div";
        if (ns >= 1e6) return QString::number(ns/1e6,'f',2) + " ms/div";
        if (ns >= 1e3) return QString::number(ns/1e3,'f',2) + " us/div";
        return QString::number(ns,'f',1) + " ns/div";
    };
    p.setPen(QColor(160, 160, 160));
    p.setFont(QFont("Monospace", 8));
    p.drawText(QRect(0, H-18, W, 16), Qt::AlignHCenter, fmt_tdiv());

    if (!session_) return;

    // Digital channel labels  --  left edge of each row
    float digi_top = H * DIG_TOP_FRAC;
    float digi_h   = H * DIG_H_FRAC;
    float row_h    = digi_h / DIG_CHANNELS;
    int n_dig = std::min((int)escope::CaptureSession::MAX_DIGITAL_CH, DIG_CHANNELS);

    int   n_vis_ov  = visibleCount(ch_visible_);
    float row_h_vis = digi_h / n_vis_ov;
    for (int ch = 0; ch < n_dig; ++ch) {
        if (!ch_visible_[ch]) continue;
        int   vr      = visibleRow(ch_visible_, ch);
        float row_top = digi_top + vr * row_h_vis;
        float row_mid = row_top + row_h_vis * 0.5f;

        // Channel label (D0-D7)
        p.setFont(QFont("Monospace", 8, QFont::Bold));
        float g = (ch % 2 == 0) ? 200.f : 150.f;
        float b = (ch % 2 == 0) ?  40.f : 115.f;
        p.setPen(QColor(25, (int)g, (int)b));
        QString label = QString::fromStdString(session_->digital_info(ch).label);
        p.drawText(QRect(4, (int)row_mid - 6, 28, 13), Qt::AlignLeft | Qt::AlignVCenter, label);

        // Show logic level at current view left edge
        bool lvl = session_->digital_buffer().level_at(ch, time_offset_ns_);
        p.setFont(QFont("Monospace", 7));
        p.setPen(lvl ? QColor(80,220,80) : QColor(120,120,120));
        p.drawText(QRect(36, (int)row_mid - 5, 16, 11), Qt::AlignCenter, lvl ? "1" : "0");
    }

    // Cursors and measurement panel
    drawCursors(p);
    drawMeasurementPanel(p);

    // Show PAUSED only when user explicitly panned (right-click drag)
    if (!follow_latest_) {
        p.setFont(QFont("Monospace", 8));
        p.setPen(QColor(200, 150, 50));
        p.drawText(QRect(W-120, 4, 116, 14), Qt::AlignRight, "PAUSED  L=live");
    }

    /* Draw rubber band selection box */
    if (rubber_band_active_) {
        int x1 = std::min(rubber_band_start_.x(), rubber_band_end_.x());
        int y1 = std::min(rubber_band_start_.y(), rubber_band_end_.y());
        int x2 = std::max(rubber_band_start_.x(), rubber_band_end_.x());
        int y2 = std::max(rubber_band_start_.y(), rubber_band_end_.y());
        p.setPen(QPen(QColor(100, 180, 255), 1, Qt::DashLine));
        p.setBrush(QColor(100, 180, 255, 30));
        p.drawRect(x1, y1, x2-x1, y2-y1);
        /* Show time range of selection */
        double t1 = pixelToTime(x1), t2 = pixelToTime(x2);
        double dt = t2 - t1;
        QString range_str;
        if (dt >= 1e9)      range_str = QString::number(dt/1e9,'f',3) + " s";
        else if (dt >= 1e6) range_str = QString::number(dt/1e6,'f',3) + " ms";
        else if (dt >= 1e3) range_str = QString::number(dt/1e3,'f',3) + " us";
        else                range_str = QString::number(dt,'f',1) + " ns";
        p.setPen(QColor(100, 180, 255));
        p.setFont(QFont("Monospace", 8));
        p.drawText(QRect(x1, y1-16, x2-x1, 14), Qt::AlignCenter, range_str);
    }

    // Help hint
    p.setPen(QColor(55, 55, 65));
    p.setFont(QFont("Sans", 7));
    p.drawText(QRect(W-420, H-14, 416, 12), Qt::AlignRight,
               "Drag=zoom  Scroll=pan  RClick=menu  Mid=drag  Del=cursors  L=live");
}

// --- Cursor drawing ---------------------------------------------------------

void WaveformWidget::drawCursors(QPainter& p) {
    if (!session_) return;
    int W = width(), H = height();

    auto fmt_t = [](double ns) -> QString {
        double a = std::abs(ns);
        if (a >= 1e9) return QString::number(ns/1e9,'f',4) + "s";
        if (a >= 1e6) return QString::number(ns/1e6,'f',3) + "ms";
        if (a >= 1e3) return QString::number(ns/1e3,'f',3) + "us";
        return QString::number(ns,'f',1) + "ns";
    };

    for (int ci = 0; ci < (int)cursors_.size(); ++ci) {
        const auto& c = cursors_[ci];
        if (!c.active || c.type != WaveformCursor::Type::Vertical) continue;

        float x = std::clamp(timeToPixel(c.t_ns), 0.f, (float)W);
        QColor cc = WaveformCursor::color(ci);
        bool is_ref    = (ci == meas_ref_);
        bool is_target = (ci == meas_target_);

        Qt::PenStyle ps = is_ref ? Qt::SolidLine : is_target ? Qt::DashDotLine : Qt::DashLine;
        p.setPen(QPen(cc, is_ref || is_target ? 2 : 1, ps));
        p.drawLine(QPointF(x, 0), QPointF(x, H - 20));

        // Numbered badge at top
        QRect badge((int)x - 9, 4, 18, 16);
        p.fillRect(badge, cc);
        p.setPen(Qt::black);
        p.setFont(QFont("Monospace", 8, QFont::Bold));
        p.drawText(badge, Qt::AlignCenter, QString::number(ci + 1));

        // Time label
        p.setPen(cc);
        p.setFont(QFont("Monospace", 7));
        QRect tbox((int)x + 2, 22, 90, 13);
        if (x + 94 > W) tbox.moveLeft((int)x - 92);
        p.fillRect(tbox, QColor(0, 0, 0, 170));
        p.drawText(tbox, Qt::AlignCenter, fmt_t(c.t_ns));

        // Logic level for each digital channel at this cursor time
        float digi_top  = H * DIG_TOP_FRAC;
        float digi_h    = H * DIG_H_FRAC;
        int   n_vis_c   = visibleCount(ch_visible_);
        float row_h     = digi_h / n_vis_c;
        int n_dig = std::min((int)escope::CaptureSession::MAX_DIGITAL_CH, DIG_CHANNELS);
        for (int ch = 0; ch < n_dig; ++ch) {
            if (!ch_visible_[ch]) continue;
            if (!session_->digital_info(ch).enabled) continue;
            bool lvl = session_->digital_buffer().level_at(ch, c.t_ns);
            float row_mid = digi_top + visibleRow(ch_visible_,ch) * row_h + row_h * 0.5f;
            p.setPen(Qt::NoPen);
            p.setBrush(lvl ? QColor(cc.red(),cc.green(),cc.blue(),160) : QColor(0,0,0,0));
            if (lvl) p.drawEllipse(QPointF(x, row_mid), 3, 3);
        }
    }
}

// --- Measurement panel ------------------------------------------------------

void WaveformWidget::drawMeasurementPanel(QPainter& p) {
    if (meas_ref_ < 0 || meas_target_ < 0) return;
    if (meas_ref_ >= (int)cursors_.size() || meas_target_ >= (int)cursors_.size()) return;
    const auto& ref = cursors_[meas_ref_];
    const auto& tgt = cursors_[meas_target_];
    if (!ref.active || !tgt.active) return;
    if (ref.type != WaveformCursor::Type::Vertical || tgt.type != WaveformCursor::Type::Vertical) return;

    int W = width();
    double dt = tgt.t_ns - ref.t_ns;
    double f  = (dt != 0.0) ? 1e9 / std::abs(dt) : 0.0;

    auto fmt_dt = [](double ns) -> QString {
        double a = std::abs(ns);
        if (a >= 1e9) return QString::number(ns/1e9,'f',4) + " s";
        if (a >= 1e6) return QString::number(ns/1e6,'f',3) + " ms";
        if (a >= 1e3) return QString::number(ns/1e3,'f',3) + " us";
        return QString::number(ns,'f',1) + " ns";
    };
    auto fmt_f = [](double f) -> QString {
        if (f >= 1e6) return QString::number(f/1e6,'f',3) + " MHz";
        if (f >= 1e3) return QString::number(f/1e3,'f',3) + " kHz";
        return QString::number(f,'f',2) + " Hz";
    };

    // Logic levels at each cursor for each channel
    int n_dig = std::min((int)escope::CaptureSession::MAX_DIGITAL_CH, DIG_CHANNELS);
    QStringList lines;
    lines << QString("dT = %1   f = %2").arg(fmt_dt(dt)).arg(fmt_f(f));

    // Show which channels changed state between cursors
    QStringList changed, same_hi, same_lo;
    for (int ch = 0; ch < n_dig; ++ch) {
        if (!session_ || !session_->digital_info(ch).enabled) continue;
        bool l_ref = session_->digital_buffer().level_at(ch, ref.t_ns);
        bool l_tgt = session_->digital_buffer().level_at(ch, tgt.t_ns);
        QString lbl = QString::fromStdString(session_->digital_info(ch).label);
        if (l_ref != l_tgt)
            changed << lbl + (l_tgt ? ":0>1" : ":1>0");
        else if (l_ref)
            same_hi << lbl;
        else
            same_lo << lbl;
    }
    if (!changed.isEmpty())
        lines << "Transitions: " + changed.join("  ");
    if (!same_hi.isEmpty())
        lines << "High: " + same_hi.join(" ");
    if (!same_lo.isEmpty())
        lines << "Low:  " + same_lo.join(" ");

    // Draw panel
    p.setFont(QFont("Monospace", 9));
    int lh = 16, pad = 6;
    int bh = lines.size() * lh + pad * 2;
    QRect box(W/2 - 260, 2, 520, bh);
    p.fillRect(box, QColor(6, 8, 16, 215));
    p.setPen(QColor(50, 60, 100));
    p.drawRect(box);

    QColor rc = WaveformCursor::color(meas_ref_);
    QColor tc = WaveformCursor::color(meas_target_);
    p.fillRect(QRect(box.left()+3, box.top()+3, 5, bh-6), rc.darker(150));
    p.fillRect(QRect(box.right()-8, box.top()+3, 5, bh-6), tc.darker(150));

    p.setPen(QColor(200, 210, 255));
    for (int i = 0; i < lines.size(); ++i) {
        QRect r = box.adjusted(12, pad + i*lh, -12, 0);
        p.drawText(r, Qt::AlignLeft, lines[i]);
    }
}

// --- Coordinate mapping -----------------------------------------------------

double WaveformWidget::pixelToTime(float x) const {
    return time_offset_ns_ + (double)x / width() * time_per_div_ns_ * HDIVS;
}
float WaveformWidget::timeToPixel(double t) const {
    return (float)((t - time_offset_ns_) / (time_per_div_ns_ * HDIVS) * width());
}

#if ANALOG_ENABLED
float WaveformWidget::voltToPixel(float v, int ch) const {
    auto lay = analogLayout(ch);
    return lay.mid - (v - v_offset_[ch]) * lay.px_per_volt;
}
float WaveformWidget::pixelToVolt(float y, int ch) const {
    auto lay = analogLayout(ch);
    return v_offset_[ch] - (y - lay.mid) / lay.px_per_volt;
}
void WaveformWidget::updateCursorVoltages(WaveformCursor& c) {
    for (int ch = 0; ch < 2; ++ch) {
        const auto& s = snap_[ch];
        if (s.empty()) { c.v_ch[ch] = 0; continue; }
        auto it = std::lower_bound(s.begin(), s.end(), c.t_ns,
            [](const escope::AnalogSample& a, double t){ return a.timestamp_ns < t; });
        if (it == s.end()) c.v_ch[ch] = s.back().voltage;
        else if (it == s.begin()) c.v_ch[ch] = it->voltage;
        else c.v_ch[ch] = std::prev(it)->voltage;
    }
}
#else
float WaveformWidget::voltToPixel(float, int) const { return 0; }
float WaveformWidget::pixelToVolt(float, int) const { return 0; }
void  WaveformWidget::updateCursorVoltages(WaveformCursor&) {}
#endif

// --- Cursor API -------------------------------------------------------------

int WaveformWidget::activeCursorCount() const {
    return (int)std::count_if(cursors_.begin(), cursors_.end(),
        [](const WaveformCursor& c){ return c.active; });
}

int WaveformWidget::addCursorAtCenter() {
    double center_ns = time_offset_ns_ + (time_per_div_ns_ * HDIVS) / 2.0;
    return addVerticalCursor(center_ns);
}

int WaveformWidget::addVerticalCursor(double t_ns) {
    // Reuse inactive slot first
    for (int i = 0; i < (int)cursors_.size(); ++i) {
        if (!cursors_[i].active) {
            cursors_[i] = {true, WaveformCursor::Type::Vertical, t_ns, 0, 0, {0,0}};
            if (meas_ref_ < 0)              meas_ref_    = i;
            else if (meas_target_ < 0)      meas_target_ = i;
            emit cursorsChanged(); update(); return i;
        }
    }
    if ((int)cursors_.size() < MAX_CURSORS) {
        int i = cursors_.size();
        cursors_.push_back({true, WaveformCursor::Type::Vertical, t_ns, 0, 0, {0,0}});
        if (meas_ref_ < 0)         meas_ref_    = i;
        else if (meas_target_ < 0) meas_target_ = i;
        emit cursorsChanged(); update(); return i;
    }
    return -1;
}

int WaveformWidget::addHorizontalCursor(float v, int ch) {
#if ANALOG_ENABLED
    for (int i = 0; i < (int)cursors_.size(); ++i)
        if (!cursors_[i].active) {
            cursors_[i] = {true, WaveformCursor::Type::Horizontal, 0, v, ch, {0,0}};
            if (meas_ref_ < 0)         meas_ref_    = i;
            else if (meas_target_ < 0) meas_target_ = i;
            emit cursorsChanged(); update(); return i;
        }
    if ((int)cursors_.size() < MAX_CURSORS) {
        int i = cursors_.size();
        cursors_.push_back({true, WaveformCursor::Type::Horizontal, 0, v, ch, {0,0}});
        if (meas_ref_ < 0)         meas_ref_    = i;
        else if (meas_target_ < 0) meas_target_ = i;
        emit cursorsChanged(); update(); return i;
    }
    return -1;
#else
    (void)v; (void)ch;
    return -1; // horizontal cursors require analog channels
#endif
}

void WaveformWidget::removeCursor(int idx) {
    if (idx < 0 || idx >= (int)cursors_.size()) return;
    cursors_[idx].active = false;
    if (meas_ref_ == idx)    meas_ref_    = -1;
    if (meas_target_ == idx) meas_target_ = -1;
    // Re-assign pair from remaining active cursors
    meas_ref_ = meas_target_ = -1;
    for (int i = 0; i < (int)cursors_.size(); ++i)
        if (cursors_[i].active) {
            if (meas_ref_ < 0)         meas_ref_    = i;
            else if (meas_target_ < 0) { meas_target_ = i; break; }
        }
    emit cursorsChanged(); update();
}

void WaveformWidget::clearCursors() {
    for (auto& c : cursors_) c.active = false;
    meas_ref_ = meas_target_ = -1;
    emit cursorsChanged(); update();
}

void WaveformWidget::setMeasurementPair(int ref, int target) {
    meas_ref_ = ref; meas_target_ = target; update();
}

// --- Mouse ------------------------------------------------------------------

void WaveformWidget::mousePressEvent(QMouseEvent* e) {
    float mx = e->position().x(), my = e->position().y();

    if (e->button() == Qt::LeftButton) {
        int hit = cursorHitTest(mx, my);
        if (hit >= 0) {
            drag_cursor_   = hit;
            follow_latest_ = false;
            // Badge click: reassign measurement pair
            const auto& c = cursors_[hit];
            if (c.type == WaveformCursor::Type::Vertical) {
                float cx = timeToPixel(c.t_ns);
                bool on_badge = (mx >= cx-9 && mx <= cx+9 && my >= 4 && my <= 20);
                if (on_badge) {
                    if (meas_ref_ == hit)        std::swap(meas_ref_, meas_target_);
                    else if (meas_target_ == hit) std::swap(meas_ref_, meas_target_);
                    else                          meas_target_ = hit;
                    drag_cursor_ = -1;
                    update(); return;
                }
            }
            update(); return;
        }
        // Clicking empty area no longer drops a cursor -- use the "Add
        // Cursor" toolbar button instead (addCursorAtCenter()).
        return;
    }

    if (e->button() == Qt::RightButton) {
        int hit = cursorHitTest(mx, my);
        if (hit >= 0) { removeCursor(hit); return; }
        // Right-click empty area = pan
        follow_latest_ = false;
        dragging_ = true; drag_start_ = e->pos(); drag_t0_ = time_offset_ns_;
    }

    if (e->button() == Qt::MiddleButton) {
        follow_latest_ = false;
        dragging_ = true; drag_start_ = e->pos(); drag_t0_ = time_offset_ns_;
    }
    if (e->button() == Qt::LeftButton && dragging_) {
        dragging_ = false;
    }
}

void WaveformWidget::mouseMoveEvent(QMouseEvent* e) {
    float mx = e->position().x(), my = e->position().y(); (void)my;
    if (drag_cursor_ >= 0 && drag_cursor_ < (int)cursors_.size()) {
        auto& c = cursors_[drag_cursor_];
        if (c.type == WaveformCursor::Type::Vertical)
            c.t_ns = pixelToTime(mx);
#if ANALOG_ENABLED
        else c.v_volts = pixelToVolt(my, std::clamp(c.channel,0,1));
#endif
        update(); return;
    }
    if (dragging_) {
        double ppns = (double)width() / (time_per_div_ns_ * HDIVS);
        time_offset_ns_ = drag_t0_ + (drag_start_.x()-e->pos().x())/ppns;
        clampTimeOffset();
        update();
    }
}

void WaveformWidget::mouseReleaseEvent(QMouseEvent* e) {
    if (rubber_band_active_ && e->button() == Qt::LeftButton) {
        rubber_band_active_ = false;
        int x1 = std::min(rubber_band_start_.x(), rubber_band_end_.x());
        int x2 = std::max(rubber_band_start_.x(), rubber_band_end_.x());
        /* Only zoom if box is at least 10px wide */
        if (x2 - x1 > 10) {
            double t1 = pixelToTime(x1);
            double t2 = pixelToTime(x2);
            double range = t2 - t1;
            time_per_div_ns_ = std::max(range / HDIVS, 100.0);
            time_offset_ns_  = t1;
            follow_latest_   = false;
            clampTimeOffset();
            /* Sync T/div dropdown via signal */
            emit timeDivChanged(time_per_div_ns_);
        }
        update();
        return;
    }
    dragging_ = false; drag_cursor_ = -1;
}

void WaveformWidget::wheelEvent(QWheelEvent* e) {
    /* Scroll pans left/right only -- it must never change T/div (zoom).
     * Panning is clamped to the captured data's [t0,t1] range so the
     * signal can't be scrolled out of view entirely. */
    follow_latest_ = false;
    double step = time_per_div_ns_ * 2.0;
    double dir  = (e->angleDelta().y() > 0) ? -1.0 : 1.0;
    time_offset_ns_ += dir * step;
    clampTimeOffset();
    update();
}

void WaveformWidget::setTimePerDiv(double ns) {
    time_per_div_ns_ = ns;
    /* Picking a wider T/div while paused (not follow_latest_) leaves
     * time_offset_ns_ wherever it was; if the new, wider span now runs past
     * the actual captured data, only the fraction of the screen the data
     * still covers shows the waveform and the rest sits empty. Re-clamp so
     * the view always sits over real data. */
    clampTimeOffset();
    update();
    emit timeDivChanged(time_per_div_ns_);
}

void WaveformWidget::clampTimeOffset() {
    double lo = 0.0, hi = 0.0;
    if (session_) {
        auto [t0, t1] = session_->digital_buffer().time_range_ns();
        if (t1 > t0) { lo = t0; hi = t1; }
    }
    double view_span = time_per_div_ns_ * HDIVS;
    /* Left edge: never scroll before the data start.
     * Right edge: never scroll past the point where the data's end
     * would leave the visible window entirely. */
    double max_offset = std::max(lo, hi - view_span);
    time_offset_ns_ = std::clamp(time_offset_ns_, lo, max_offset);
}

void WaveformWidget::keyPressEvent(QKeyEvent* e) {
    switch (e->key()) {
    case Qt::Key_F:                 zoomFit();     break;
    case Qt::Key_Plus:
    case Qt::Key_Equal:             zoomInTime();  break;
    case Qt::Key_Minus:             zoomOutTime(); break;
    case Qt::Key_Left:              panLeft();     break;
    case Qt::Key_Right:             panRight();    break;
    case Qt::Key_L:                 follow_latest_ = true; update(); break;
    case Qt::Key_Delete:
    case Qt::Key_Backspace:         clearCursors(); break;
    default: QOpenGLWidget::keyPressEvent(e);
    }
}

// --- View controls ----------------------------------------------------------

void WaveformWidget::zoomInTime()  { time_per_div_ns_ = std::max(time_per_div_ns_*0.75, 1.0);    update(); }
void WaveformWidget::zoomOutTime() { time_per_div_ns_ = std::min(time_per_div_ns_*1.333, 1e12);  update(); }

void WaveformWidget::zoomFit() {
    if (!session_) return;
    auto [t0,t1] = session_->digital_buffer().time_range_ns();
    if (t1 <= t0) return;
    /* Show full range from t0 to t1, starting at t0 */
    double range = t1 - t0;
    time_per_div_ns_ = std::max(range / HDIVS, 100.0);
    time_offset_ns_  = t0;
    follow_latest_   = false;
    update();
}

void WaveformWidget::panLeft()  { time_offset_ns_ -= time_per_div_ns_*2; clampTimeOffset(); update(); }
void WaveformWidget::panRight() { time_offset_ns_ += time_per_div_ns_*2; clampTimeOffset(); update(); }

// Stubs  --  not used in digital-only mode
void WaveformWidget::setVoltPerDiv(std::size_t, float)  {}
void WaveformWidget::autoScale(std::size_t)             {}
void WaveformWidget::autoScaleAll()                     {}

QImage WaveformWidget::grabScreenshot() {
    makeCurrent();
    QImage img = grabFramebuffer();
    doneCurrent();
    return img;
}
