// WaveformWidget.cpp
// Digital-only mode (Phase 1/2).
// Analog code is preserved under #if ANALOG_ENABLED blocks  --  set to 1 to restore.

#define ANALOG_ENABLED 0

#include "waveform/WaveformWidget.h"
#include <QToolTip>
#include "waveform/AnnotationPainter.h"
#include <QFontMetrics>
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
    setMouseTracking(true);          // annotation tooltips follow the mouse
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
    // Rows are shared among the VISIBLE channels, the same layout the channel
    // labels, row separators and cursor dots use (this used to index by channel
    // number, which put the traces in a different place than their labels).
    const float H        = height();
    const float digi_top = H * DIG_TOP_FRAC;
    const float digi_h   = H * DIG_H_FRAC;
    const int   n_vis    = visibleCount(ch_visible_);
    const float row_h    = digi_h / n_vis;
    const float top      = digi_top + visibleRow(ch_visible_, (int)ch) * row_h;

    // A channel with decoded annotations keeps a lane under its trace.
    const float lane   = hasAnnotations((int)ch) ? ANNOT_LANE_H + 14.f : 0.f;
    const float usable = row_h - lane;
    const float amp    = std::clamp(usable * 0.55f, 24.f, 80.f);   // trace height
    const float mid    = top + usable * 0.5f;
    return hi ? mid - amp * 0.5f : mid + amp * 0.5f;
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
    glClearColor(theme::kCanvas.redF(), theme::kCanvas.greenF(), theme::kCanvas.blueF(), 1.f);
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
        const double span = time_per_div_ns_ * HDIVS;
        double anchor = t1;
        /* Sparse/bursty signals (e.g. UART with idle gaps) can leave the
         * newest window with no edges on the channels being shown, which
         * draws as a blank flat line. t1 is the newest edge on ANY channel
         * (other channels may toggle continuously), so check only the
         * visible ones and, if they've all been idle for longer than the
         * window, anchor to their latest edge instead. */
        double last_vis = -1;
        for (uint8_t c = 0; c < 8; ++c) {
            if (!ch_visible_[c]) continue;
            auto e = session_->digital_buffer().last_edges(c, 1);
            if (!e.empty()) last_vis = std::max(last_vis, e.back().timestamp_ns);
        }
        if (last_vis > 0 && last_vis < t1 - span) anchor = last_vis + span * 0.1;
        time_offset_ns_ = std::max(0.0, anchor - span);

        // Triggered display: when the newest burst holds enough data after its trigger to fill the
        // screen, put the trigger just inside the left edge. Each burst then starts at the same
        // place and fills the view, instead of showing whatever idle tail happened to be last.
        const double trig = session_->digital_buffer().last_trigger_ns();
        if (trig >= 0.0) {
            const double pre   = span * 0.05;
            const double start = std::max(0.0, trig - pre);
            if (start + span <= t1) time_offset_ns_ = start;
        }
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
    // Gate on ch_visible_ (the toolbar "Select" checkboxes the user actually
    // toggles), not just digital_info(ch).enabled -- previously this loop
    // ignored ch_visible_ entirely, so every channel was decoded into GL
    // vertices and drawn regardless of what was unchecked in the channel
    // selector, defeating the point of hiding channels to cut processing.
    int n_dig = std::min({(int)escope::CaptureSession::MAX_DIGITAL_CH, DIG_CHANNELS, 8});
    const bool has_data = session_->digital_buffer().total_edges() > 0;   // no baseline on an empty screen
    for (int ch = 0; ch < n_dig; ++ch)
        if (has_data && session_->digital_info(ch).enabled && ch_visible_[ch])
            drawDigitalChannel(ch);

    drawOverlay();
}

// --- Grid -------------------------------------------------------------------

void WaveformWidget::drawGrid() {
    float W = width(), H = height();
    shader_.bind();
    QMatrix4x4 mvp; mvp.ortho(0, W, H, 0, -1, 1);
    shader_.setUniformValue("mvp", mvp);
    shader_.setUniformValue("color", QVector4D(theme::kGrid.redF(), theme::kGrid.greenF(), theme::kGrid.blueF(), 1.f));
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
    glLineWidth(1.6f);
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

    // Edges that land in the same pixel column are merged into one vertical bar. A dense signal
    // (an 8 MHz clock zoomed out) then costs at most a few vertices per column instead of
    // overflowing the vertex buffer, which used to cut the trace off partway across the screen.
    int   col = -1, count = 0;
    float first_x = 0, last_y = yp;
    auto flush = [&]() {
        if (count == 0) return;
        if (count == 1) {
            wave_verts_.insert(wave_verts_.end(), {xp, yp, first_x, yp, first_x, yp, first_x, last_y});
            xp = first_x;
        } else {
            const float cx = (float)col + 0.5f;
            wave_verts_.insert(wave_verts_.end(), {xp, yp, cx, yp, cx, lo_y, cx, hi_y});
            xp = cx;
        }
        yp = last_y;
        count = 0;
    };

    for (const auto& e : dig_snap_) {
        if (e.channel != (uint8_t)ch) continue;
        if (e.timestamp_ns < vs) { yp = e.rising ? hi_y : lo_y; last_y = yp; xp = 0; continue; }
        if (e.timestamp_ns > ve) break;
        const float x = timeToPixel(e.timestamp_ns);
        const int   c = (int)x;
        if (count > 0 && c != col) flush();
        if (count == 0) { col = c; first_x = x; }
        ++count;
        last_y = e.rising ? hi_y : lo_y;
    }
    flush();
    wave_verts_.insert(wave_verts_.end(), {xp, yp, (float)W, yp});
    if (wave_verts_.empty()) return;

    const QColor trace = theme::channel((int)ch);

    shader_.bind();
    QMatrix4x4 mvp; mvp.ortho(0, W, H, 0, -1, 1);
    shader_.setUniformValue("mvp", mvp);
    shader_.setUniformValue("color", QVector4D(trace.redF(), trace.greenF(), trace.blueF(), 1.f));
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
    p.setRenderHint(QPainter::Antialiasing, true);
    int W = width(), H = height();

    // Time/div label (bottom centre, above digital area)
    auto fmt_tdiv = [&]() -> QString {
        double ns = time_per_div_ns_;
        if (ns >= 1e9) return QString::number(ns/1e9,'f',2) + " s/div";
        if (ns >= 1e6) return QString::number(ns/1e6,'f',2) + " ms/div";
        if (ns >= 1e3) return QString::number(ns/1e3,'f',2) + " \u00b5s/div";
        return QString::number(ns,'f',1) + " ns/div";
    };
    p.setPen(theme::kTextMuted);
    p.setFont(theme::mono(8.5));
    p.drawText(QRect(0, H-22, W, 18), Qt::AlignHCenter | Qt::AlignVCenter, fmt_tdiv());

    if (!session_) return;

    // Friendly empty state until the first capture arrives
    if (session_->digital_buffer().total_edges() == 0) {
        p.setPen(theme::kText);
        p.setFont(theme::ui(15.0, QFont::DemiBold));
        p.drawText(QRect(0, H / 2 - 44, W, 28), Qt::AlignCenter, "No capture");
        p.setPen(theme::kTextMuted);
        p.setFont(theme::ui(10.5));
        p.drawText(QRect(0, H / 2 - 12, W, 22), Qt::AlignCenter, "Connect a device, then press Space to start capturing.");
        p.setPen(theme::kTextFaint);
        p.setFont(theme::ui(9.0));
        p.drawText(QRect(0, H / 2 + 14, W, 20), Qt::AlignCenter, "Help > Controls lists every shortcut.");
    }

    // Digital channel labels  --  left edge of each row, level with its trace
    int n_dig = std::min((int)escope::CaptureSession::MAX_DIGITAL_CH, DIG_CHANNELS);
    for (int ch = 0; ch < n_dig; ++ch) {
        if (!ch_visible_[ch]) continue;
        float row_mid = (digitalRowY(ch, true) + digitalRowY(ch, false)) * 0.5f;   // centre of the trace

        // Channel tag: a colour bar, the name, and the level at the left edge of the view
        const QColor col = theme::channel(ch);
        p.setPen(Qt::NoPen);
        p.setBrush(col);
        p.drawRoundedRect(QRectF(8, row_mid - 9, 3, 18), 1.5, 1.5);
        p.setFont(theme::ui(9.0, QFont::DemiBold));
        p.setPen(col);
        QString label = QString::fromStdString(session_->digital_info(ch).label);
        p.drawText(QRect(16, (int)row_mid - 9, 34, 18), Qt::AlignLeft | Qt::AlignVCenter, label);

        bool lvl = session_->digital_buffer().level_at(ch, time_offset_ns_);
        p.setFont(theme::mono(8.0));
        p.setPen(lvl ? col : theme::kTextFaint);
        p.drawText(QRect(50, (int)row_mid - 9, 14, 18), Qt::AlignCenter, lvl ? "1" : "0");
    }

    drawAnnotations(p);

    // Hover guide: a hairline and the time under the pointer, so reading a position needs no cursor
    if (hover_pos_.x() >= 0 && !dragging_ && !rubber_band_active_ && drag_cursor_ < 0) {
        const float hx = (float)hover_pos_.x();
        p.setPen(QPen(QColor(255, 255, 255, 34), 1));
        p.drawLine(QPointF(hx, 0), QPointF(hx, H - 26));
        const double ht = pixelToTime(hx);
        QString txt;
        const double a = std::abs(ht);
        if (a >= 1e9)      txt = QString::number(ht / 1e9, 'f', 6) + " s";
        else if (a >= 1e6) txt = QString::number(ht / 1e6, 'f', 4) + " ms";
        else if (a >= 1e3) txt = QString::number(ht / 1e3, 'f', 3) + " \u00b5s";
        else               txt = QString::number(ht, 'f', 1) + " ns";
        p.setFont(theme::mono(8.5, QFont::DemiBold));
        const int tw = QFontMetrics(p.font()).horizontalAdvance(txt) + 18;
        QRectF chip(std::clamp(hx - tw / 2.0, 6.0, (double)W - tw - 6.0), H - 52, tw, 20);
        p.setPen(Qt::NoPen);
        p.setBrush(QColor(44, 44, 46, 235));
        p.drawRoundedRect(chip, 10, 10);
        p.setPen(theme::kText);
        p.drawText(chip, Qt::AlignCenter, txt);
    }

    // Cursors and measurement panel
    drawCursors(p);
    drawMeasurementPanel(p);

    // Badge shown only when the user has panned away from the live edge
    if (!follow_latest_) {
        const QString msg = "Paused. Press L for live view";
        p.setFont(theme::ui(8.5, QFont::Medium));
        const int tw = QFontMetrics(p.font()).horizontalAdvance(msg) + 22;
        const QRectF badge(W - tw - 12, 12, tw, 24);
        p.setPen(Qt::NoPen);
        p.setBrush(QColor(255, 159, 10, 36));
        p.drawRoundedRect(badge, 12, 12);
        p.setPen(theme::kOrange);
        p.drawText(badge, Qt::AlignCenter, msg);
    }

    /* Draw rubber band selection box */
    if (rubber_band_active_) {
        int x1 = std::min(rubber_band_start_.x(), rubber_band_end_.x());
        int y1 = std::min(rubber_band_start_.y(), rubber_band_end_.y());
        int x2 = std::max(rubber_band_start_.x(), rubber_band_end_.x());
        int y2 = std::max(rubber_band_start_.y(), rubber_band_end_.y());
        p.setPen(QPen(theme::kAccent, 1));
        p.setBrush(QColor(10, 132, 255, 38));
        p.drawRect(x1, y1, x2-x1, y2-y1);
        /* Show time range of selection */
        double t1 = pixelToTime(x1), t2 = pixelToTime(x2);
        double dt = t2 - t1;
        QString range_str;
        if (dt >= 1e9)      range_str = QString::number(dt/1e9,'f',3) + " s";
        else if (dt >= 1e6) range_str = QString::number(dt/1e6,'f',3) + " ms";
        else if (dt >= 1e3) range_str = QString::number(dt/1e3,'f',3) + " \u00b5s";
        else                range_str = QString::number(dt,'f',1) + " ns";
        p.setPen(theme::kAccent);
        p.setFont(theme::mono(8.5, QFont::DemiBold));
        p.drawText(QRect(x1, y1-20, x2-x1, 16), Qt::AlignCenter, range_str);
    }

    // Help hint
    p.setPen(theme::kTextFaint);
    p.setFont(theme::ui(8.0));
    p.drawText(QRect(W-560, H-22, 548, 18), Qt::AlignRight | Qt::AlignVCenter,
               "Drag to zoom    Scroll to pan    Delete clears cursors    L for live");
}

// Cursor badges sit just below the readout card
static constexpr float kBadgeY = 72.f;

// --- Cursor drawing ---------------------------------------------------------

void WaveformWidget::drawCursors(QPainter& p) {
    if (!session_) return;
    int W = width(), H = height();

    auto fmt_t = [](double ns) -> QString {
        double a = std::abs(ns);
        if (a >= 1e9) return QString::number(ns/1e9,'f',4) + " s";
        if (a >= 1e6) return QString::number(ns/1e6,'f',3) + " ms";
        if (a >= 1e3) return QString::number(ns/1e3,'f',3) + " \u00b5s";
        return QString::number(ns,'f',1) + " ns";
    };

    for (int ci = 0; ci < (int)cursors_.size(); ++ci) {
        const auto& c = cursors_[ci];
        if (!c.active || c.type != WaveformCursor::Type::Vertical) continue;

        float x = std::clamp(timeToPixel(c.t_ns), 0.f, (float)W);
        QColor cc = WaveformCursor::color(ci);
        bool is_ref    = (ci == meas_ref_);
        bool is_target = (ci == meas_target_);

        Qt::PenStyle ps = is_ref ? Qt::SolidLine : is_target ? Qt::DashDotLine : Qt::DashLine;
        p.setPen(QPen(cc, is_ref || is_target ? 1.6 : 1.0, ps));
        p.drawLine(QPointF(x, 0), QPointF(x, H - 20));

        // Numbered badge at top
        QRectF badge(x - 10, kBadgeY, 20, 18);
        p.setRenderHint(QPainter::Antialiasing, true);
        p.setPen(Qt::NoPen);
        p.setBrush(cc);
        p.drawRoundedRect(badge, 9, 9);
        p.setPen(QColor(10, 10, 12));
        p.setFont(theme::ui(8.5, QFont::Bold));
        p.drawText(badge, Qt::AlignCenter, QString::number(ci + 1));

        // Time label
        p.setFont(theme::mono(8.0, QFont::DemiBold));
        QRectF tbox(x + 6, kBadgeY + 22, 88, 18);
        if (x + 100 > W) tbox.moveLeft(x - 94);
        p.setPen(Qt::NoPen);
        p.setBrush(QColor(20, 20, 22, 210));
        p.drawRoundedRect(tbox, 6, 6);
        p.setPen(cc);
        p.drawText(tbox, Qt::AlignCenter, fmt_t(c.t_ns));

        // Logic level for each digital channel at this cursor time (a dot on the trace)
        int n_dig = std::min((int)escope::CaptureSession::MAX_DIGITAL_CH, DIG_CHANNELS);
        for (int ch = 0; ch < n_dig; ++ch) {
            if (!ch_visible_[ch]) continue;
            if (!session_->digital_info(ch).enabled) continue;
            bool lvl = session_->digital_buffer().level_at(ch, c.t_ns);
            float row_mid = digitalRowY(ch, true);
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

    const double dt = std::abs(tgt.t_ns - ref.t_ns);

    auto fmt_time = [](double ns) -> QString {
        const double a = std::abs(ns);
        if (a >= 1e9) return QString::number(ns / 1e9, 'f', 4) + " s";
        if (a >= 1e6) return QString::number(ns / 1e6, 'f', 3) + " ms";
        if (a >= 1e3) return QString::number(ns / 1e3, 'f', 3) + " \u00b5s";
        return QString::number(ns, 'f', 1) + " ns";
    };
    auto fmt_freq = [](double hz) -> QString {
        if (hz >= 1e6) return QString::number(hz / 1e6, 'f', 3) + " MHz";
        if (hz >= 1e3) return QString::number(hz / 1e3, 'f', 3) + " kHz";
        return QString::number(hz, 'f', 2) + " Hz";
    };

    // Duty cycle: share of the interval between the cursors that the first visible channel spends high
    QString duty_name = "Duty", duty_value = "N/A";
    for (int ch = 0; ch < DIG_CHANNELS && ch < 8; ++ch) {
        if (!ch_visible_[ch]) continue;
        duty_name = QString("Duty  D%1").arg(ch);
        if (dt > 0.0 && session_) {
            const double lo = std::min(ref.t_ns, tgt.t_ns), hi = std::max(ref.t_ns, tgt.t_ns);
            const double pct = 100.0 * session_->digital_buffer().high_time_ns(static_cast<uint8_t>(ch), lo, hi) / dt;
            duty_value = QString::number(pct, 'f', 1) + " %";
        }
        break;
    }

    struct Field { QString name, value; QColor accent; };
    const QColor neutral(99, 99, 102);
    const Field fields[5] = {
        { QString("X%1").arg(meas_ref_ + 1),    fmt_time(ref.t_ns), WaveformCursor::color(meas_ref_) },
        { QString("X%1").arg(meas_target_ + 1), fmt_time(tgt.t_ns), WaveformCursor::color(meas_target_) },
        { QString::fromUtf8("\u0394X"),           fmt_time(dt),       neutral },
        { QString("1/\u0394X"),                  dt > 0.0 ? fmt_freq(1e9 / dt) : QString("N/A"), neutral },
        { duty_name,                              duty_value,         neutral },
    };

    const int box_h = 50, pad = 12;
    const int col_w = std::clamp((width() - 2 * pad - 40) / 5, 96, 124);
    const int box_w = col_w * 5 + pad * 2;
    const QRect box(width() / 2 - box_w / 2, 12, box_w, box_h);

    p.save();
    p.setRenderHint(QPainter::Antialiasing, true);
    p.setPen(QPen(QColor(255, 255, 255, 22), 1));
    p.setBrush(QColor(28, 28, 30, 232));
    p.drawRoundedRect(QRectF(box).adjusted(0.5, 0.5, -0.5, -0.5), 12, 12);

    for (int i = 0; i < 5; ++i) {
        const QRect col(box.left() + pad + i * col_w, box.top(), col_w, box_h);
        if (i > 0) {
            p.setPen(QColor(255, 255, 255, 18));
            p.drawLine(col.left() - 4, box.top() + 11, col.left() - 4, box.bottom() - 11);
        }
        if (i < 2) {                                           // colour dot ties X1 and X2 to their cursors
            p.setPen(Qt::NoPen);
            p.setBrush(fields[i].accent);
            p.drawEllipse(QPointF(col.left() + 6, box.top() + 14), 3.5, 3.5);
        }
        const int lx = col.left() + (i < 2 ? 16 : 6);
        p.setFont(theme::ui(8.0, QFont::DemiBold));
        p.setPen(theme::kTextMuted);
        p.drawText(QRect(lx, box.top() + 7, col_w - 14, 15), Qt::AlignLeft | Qt::AlignVCenter, fields[i].name);
        p.setFont(theme::mono(10.5, QFont::DemiBold));
        p.setPen(theme::kText);
        p.drawText(QRect(col.left() + 6, box.top() + 24, col_w - 8, 20), Qt::AlignLeft | Qt::AlignVCenter, fields[i].value);
    }
    p.restore();
}

// --- Protocol annotations ---------------------------------------------------

void WaveformWidget::setAnnotations(int channel,
        std::shared_ptr<const std::vector<escope::DecodedEvent>> events) {
    auto it = std::find_if(lanes_.begin(), lanes_.end(),
        [&](const AnnotationLane& l) { return l.channel == channel; });
    if (it == lanes_.end()) { lanes_.push_back({channel, {}}); it = lanes_.end() - 1; }
    it->index.build(std::move(events));
    update();
}

void WaveformWidget::clearAnnotations() {
    lanes_.clear();
    hit_boxes_.clear();
    update();
}

bool WaveformWidget::hasAnnotations(int channel) const {
    return std::any_of(lanes_.begin(), lanes_.end(),
        [&](const AnnotationLane& l) { return l.channel == channel && !l.index.empty(); });
}

void WaveformWidget::drawAnnotations(QPainter& p) {
    hit_boxes_.clear();
    if (lanes_.empty() || !session_) return;

    const int    W    = width();
    const double span = time_per_div_ns_ * HDIVS;
    escope::AnnotationView view;
    view.t0_ns     = time_offset_ns_;
    view.t1_ns     = time_offset_ns_ + span;
    view.px_per_ns = W / span;

    AnnotationLaneGeometry g;
    g.t0_ns     = time_offset_ns_;
    g.px_per_ns = W / span;
    g.width     = W;
    g.height    = ANNOT_LANE_H;

    for (const auto& lane : lanes_) {
        if (lane.channel < 0 || lane.channel >= 8 || !ch_visible_[lane.channel]) continue;
        g.y = digitalRowY(lane.channel, false) + 10.f;
        std::vector<AnnotationHit> hits;
        paintAnnotationLane(p, lane.index.items(view), g, hover_pos_, &hits);
        for (auto& h : hits) hit_boxes_.push_back({h.rect, h.tip});
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

bool WaveformWidget::autoScaleTimeDiv() {
    if (!session_) return false;

    // Prefer the first visible channel (what the user's actually looking
    // at); fall back to D0 if nothing is currently shown.
    int ch = -1;
    for (int i = 0; i < 8; i++) if (ch_visible_[i]) { ch = i; break; }
    if (ch < 0) ch = 0;

    // Measure the median rising-to-rising period from the most recent
    // edges -- same approach ChannelPanel/MeasurementPanel use for their
    // frequency readouts, so this agrees with what's shown there.
    auto edges = session_->digital_buffer().last_edges(ch, 50);
    if (edges.size() < 2) return false;

    std::vector<double> periods;
    double prev_rise = -1;
    for (const auto& e : edges) {
        if (e.rising) {
            if (prev_rise > 0) periods.push_back(e.timestamp_ns - prev_rise);
            prev_rise = e.timestamp_ns;
        }
    }

    // Spacing between consecutive edges of any polarity. For serial data
    // (UART etc.) the shortest of these is one bit time.
    std::vector<double> gaps;
    for (size_t i = 1; i < edges.size(); ++i) {
        double g = edges[i].timestamp_ns - edges[i - 1].timestamp_ns;
        if (g > 0.0) gaps.push_back(g);
    }
    if (gaps.empty()) return false;
    std::sort(gaps.begin(), gaps.end());

    // Periodic if rising-edge periods are tightly clustered; otherwise the
    // signal is data-dependent (e.g. UART) and the median period is
    // meaningless -- scale from the bit time instead.
    bool periodic = false;
    double period_ns = 0.0;
    if (periods.size() >= 2) {
        std::sort(periods.begin(), periods.end());
        period_ns = periods[periods.size() / 2];
        periodic = period_ns > 0.0 &&
                   periods.back() <= period_ns * 1.25 &&
                   periods.front() >= period_ns * 0.8;
    }

    double ns;
    if (periodic) {
        // Fit a handful of cycles across the full screen width so the
        // waveform's shape is legible.
        constexpr double CYCLES_ACROSS_SCREEN = 3.0;
        ns = period_ns * CYCLES_ACROSS_SCREEN / HDIVS;
    } else {
        // 10th-percentile gap rejects stray glitches; show several
        // 10-bit UART frames across the screen.
        constexpr double BITS_ACROSS_SCREEN = 100.0;
        double bit_ns = gaps[gaps.size() / 10];
        ns = bit_ns * BITS_ACROSS_SCREEN / HDIVS;
    }
    setTimePerDiv(std::max(1.0, ns));
    return true;
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
                bool on_badge = (mx >= cx-11 && mx <= cx+11 && my >= kBadgeY - 2 && my <= kBadgeY + 20);
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
        // Dragging from empty space draws a zoom rectangle (applied on release).
        rubber_band_active_ = true;
        rubber_band_start_ = rubber_band_end_ = e->pos();
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
    hover_pos_ = e->position();
    if (!dragging_ && drag_cursor_ < 0 && !rubber_band_active_) {
        bool over = false;
        for (const auto& hb : hit_boxes_)
            if (hb.rect.contains(hover_pos_)) {
                QToolTip::showText(e->globalPosition().toPoint(), hb.tip, this);
                over = true;
                break;
            }
        if (!over) QToolTip::hideText();
        update();                               // hover outline
    }
    if (drag_cursor_ >= 0 && drag_cursor_ < (int)cursors_.size()) {
        auto& c = cursors_[drag_cursor_];
        if (c.type == WaveformCursor::Type::Vertical)
            c.t_ns = pixelToTime(mx);
#if ANALOG_ENABLED
        else c.v_volts = pixelToVolt(my, std::clamp(c.channel,0,1));
#endif
        update(); return;
    }
    if (rubber_band_active_) {
        rubber_band_end_ = e->pos();
        update();
        return;
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
            follow_latest_ = false;
            animateView(std::max(range / HDIVS, 10.0), t1);
        }
        update();
        return;
    }
    dragging_ = false; drag_cursor_ = -1;
}

void WaveformWidget::leaveEvent(QEvent* e) {
    hover_pos_ = QPointF(-1, -1);
    QToolTip::hideText();
    update();
    QOpenGLWidget::leaveEvent(e);
}

void WaveformWidget::wheelEvent(QWheelEvent* e) {
    // Ctrl + scroll zooms about the pointer, the way maps and design tools do
    if (e->modifiers() & Qt::ControlModifier) {
        const double steps = e->angleDelta().y() / 120.0;
        if (steps != 0.0) {
            if (view_anim_) view_anim_->stop();
            const double mx = e->position().x();
            const double tm = pixelToTime((float)mx);
            const double nt = std::clamp(time_per_div_ns_ * std::pow(0.8, steps), 1.0, 1e12);
            follow_latest_   = false;
            time_per_div_ns_ = nt;
            time_offset_ns_  = tm - (mx / std::max(1, width())) * nt * HDIVS;
            clampTimeOffset();
            emit timeDivChanged(time_per_div_ns_);
            update();
        }
        e->accept();
        return;
    }
    /* Scroll pans left/right only -- it must never change T/div (zoom).
     * The step is proportional to the actual scroll amount (a touchpad sends
     * many tiny deltas; a wheel notch is 120 units = one division), and
     * horizontal scrolling is honoured too. Scrolling up/left moves earlier. */
    follow_latest_ = false;
    const QPoint pd = e->pixelDelta();
    const QPoint ad = e->angleDelta();
    double dt;
    if (!pd.isNull()) {
        const double px   = pd.x() != 0 ? pd.x() : pd.y();
        const double ppns = (double)width() / (time_per_div_ns_ * HDIVS);
        dt = -px / ppns;
    } else {
        const double units = ad.x() != 0 ? ad.x() : ad.y();
        dt = -units / 120.0 * time_per_div_ns_;
    }
    time_offset_ns_ += dt;
    clampTimeOffset();
    update();
    e->accept();
}

bool WaveformWidget::snapToSignal() {
    if (!session_) return false;
    const auto& buf = session_->digital_buffer();
    const double span = time_per_div_ns_ * HDIVS;
    double newest = -1;
    for (uint8_t c = 0; c < 8; ++c) {
        if (!ch_visible_[c]) continue;
        auto last = buf.last_edges(c, 1);
        if (last.empty()) continue;
        newest = std::max(newest, last.back().timestamp_ns);
        for (const auto& ed : buf.edges_in_range(time_offset_ns_, time_offset_ns_ + span))
            if (ed.channel == c) return false;       // signal already on screen
    }
    if (newest < 0) return false;
    time_offset_ns_ = newest - span * 0.75;
    clampTimeOffset();
    update();
    return true;
}

void WaveformWidget::jumpToEdge(bool forward) {
    if (!session_) return;
    follow_latest_ = false;
    const auto& buf = session_->digital_buffer();
    const double span   = time_per_div_ns_ * HDIVS;
    const double center = time_offset_ns_ + span / 2.0;
    const double eps    = std::max(1.0, time_per_div_ns_ * 0.01);
    const auto [t0, t1] = buf.time_range_ns();
    const auto edges = forward ? buf.edges_in_range(center + eps, t1)
                               : buf.edges_in_range(t0, center - eps);
    double target = -1;
    if (forward) {
        for (const auto& ed : edges)
            if (ch_visible_[ed.channel]) { target = ed.timestamp_ns; break; }
    } else {
        for (auto it = edges.rbegin(); it != edges.rend(); ++it)
            if (ch_visible_[it->channel]) { target = it->timestamp_ns; break; }
    }
    if (target < 0) return;
    time_offset_ns_ = target - span / 2.0;
    clampTimeOffset();
    update();
}

void WaveformWidget::setTimePerDiv(double ns) {
    // Glide to the new scale about the middle of the view; the end of the glide re-clamps the
    // offset so a wider span never leaves the screen partly empty.
    const double nt = std::clamp(ns, 1.0, 1e12);
    animateView(nt, time_offset_ns_ + time_per_div_ns_ * HDIVS / 2.0 - nt * HDIVS / 2.0);
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
    case Qt::Key_BracketRight:      jumpToEdge(true);  break;
    case Qt::Key_BracketLeft:       jumpToEdge(false); break;
    case Qt::Key_Home:              follow_latest_ = false; time_offset_ns_ = 0; clampTimeOffset(); update(); break;
    case Qt::Key_End:               follow_latest_ = false; time_offset_ns_ = 1e30; clampTimeOffset(); update(); break;
    case Qt::Key_L:                 follow_latest_ = true; update(); break;
    case Qt::Key_Delete:
    case Qt::Key_Backspace:         clearCursors(); break;
    default: QOpenGLWidget::keyPressEvent(e);
    }
}

// --- View controls ----------------------------------------------------------

// Zoom keeps the centre of the view fixed. Repeated presses build on the pending target so a
// held key keeps accelerating instead of restarting the glide each time.
void WaveformWidget::zoomInTime() {
    const bool run = view_anim_ && view_anim_->state() == QAbstractAnimation::Running;
    const double cur = run ? anim_to_tdiv_ : time_per_div_ns_, off = run ? anim_to_off_ : time_offset_ns_;
    const double nt = std::max(cur * 0.7, 1.0);
    animateView(nt, off + cur * HDIVS / 2.0 - nt * HDIVS / 2.0);
}
void WaveformWidget::zoomOutTime() {
    const bool run = view_anim_ && view_anim_->state() == QAbstractAnimation::Running;
    const double cur = run ? anim_to_tdiv_ : time_per_div_ns_, off = run ? anim_to_off_ : time_offset_ns_;
    const double nt = std::min(cur / 0.7, 1e12);
    animateView(nt, off + cur * HDIVS / 2.0 - nt * HDIVS / 2.0);
}

void WaveformWidget::animateView(double tdiv, double offset) {
    anim_from_tdiv_ = time_per_div_ns_;  anim_from_off_ = time_offset_ns_;
    anim_to_tdiv_   = std::clamp(tdiv, 1.0, 1e12);  anim_to_off_ = offset;
    if (!view_anim_) {
        view_anim_ = new QVariantAnimation(this);
        view_anim_->setDuration(180);
        view_anim_->setEasingCurve(QEasingCurve::OutCubic);
        view_anim_->setStartValue(0.0);
        view_anim_->setEndValue(1.0);
        connect(view_anim_, &QVariantAnimation::valueChanged, this, [this](const QVariant& v) {
            const double t = v.toDouble();
            // Zoom in log space so each frame scales by the same factor
            time_per_div_ns_ = std::exp(std::log(anim_from_tdiv_) * (1.0 - t) + std::log(anim_to_tdiv_) * t);
            time_offset_ns_  = anim_from_off_ + (anim_to_off_ - anim_from_off_) * t;
            emit timeDivChanged(time_per_div_ns_);
            update();
        });
        connect(view_anim_, &QVariantAnimation::finished, this, [this] {
            time_per_div_ns_ = anim_to_tdiv_;
            if (!follow_latest_) clampTimeOffset();
            emit timeDivChanged(time_per_div_ns_);
            update();
        });
    }
    view_anim_->stop();
    view_anim_->start();
}

void WaveformWidget::zoomFit() {
    if (!session_) return;
    auto [t0,t1] = session_->digital_buffer().time_range_ns();
    if (t1 <= t0) return;
    /* Show full range from t0 to t1, starting at t0 */
    double range = t1 - t0;
    follow_latest_ = false;
    animateView(std::max(range / HDIVS, 100.0), t0);
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
