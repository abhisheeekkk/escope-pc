#pragma once
#include <QDateTime>

#include <QOpenGLWidget>
#include <QOpenGLFunctions>
#include <QOpenGLShaderProgram>
#include <QOpenGLBuffer>
#include <QOpenGLVertexArrayObject>
#include <QPainter>
#include <QImage>
#include <QColor>
#include "theme/Theme.h"
#include <QVariantAnimation>
#include <QElapsedTimer>
#include <QEasingCurve>

#include "acquisition/SampleBuffer.h"
#include "acquisition/DigitalBuffer.h"
#include "decoders/base/AnnotationLayout.h"

#include <memory>
#include <vector>
#include <array>

namespace escope { class CaptureSession; }

// ─── WaveformCursor ───────────────────────────────────────────────────────────

struct WaveformCursor {
    enum class Type { Vertical, Horizontal };

    bool   active  = false;
    Type   type    = Type::Vertical;
    double t_ns    = 0.0;      ///< Time position  (Vertical cursors)
    float  v_volts = 0.f;      ///< Voltage position (Horizontal cursors)
    int    channel = 0;        ///< Channel for horizontal cursor
    float  v_ch[2] = {0, 0};  ///< Sampled voltages (filled each frame for vertical)

    static constexpr int MAX_CURSORS = 8;

    static QColor color(int idx) { return theme::cursor(idx); }
};

// ─── WaveformWidget ───────────────────────────────────────────────────────────

class WaveformWidget : public QOpenGLWidget, protected QOpenGLFunctions {
    Q_OBJECT

public:
    static constexpr int VDIVS_PER_CH = 4;
    static constexpr int HDIVS        = 10;
    static constexpr int MAX_CURSORS  = WaveformCursor::MAX_CURSORS;

    explicit WaveformWidget(QWidget* parent = nullptr);
    ~WaveformWidget() override;

    void setSession(const escope::CaptureSession* session);

    // ── View controls ─────────────────────────────────────────────────────────
    void   zoomInTime();
    void   zoomOutTime();
    void   zoomFit();
    void   panLeft();
    void   panRight();
    void   setVoltPerDiv(std::size_t ch, float v);
    float  voltPerDiv(std::size_t ch) const { return volt_per_div_[ch]; }
    double timePerDiv()               const { return time_per_div_ns_; }
    double timeOffset()               const { return time_offset_ns_; }
    void   autoScale(std::size_t ch);
    void   autoScaleAll();

    // ── Cursor API ────────────────────────────────────────────────────────────
    int  addVerticalCursor(double t_ns);
    int  addHorizontalCursor(float v_volts, int ch = 0);
    /// Drop a vertical cursor at the center of the currently visible window
    /// (used by the toolbar "Add Cursor" button, replacing click-to-drop).
    int  addCursorAtCenter();
    void removeCursor(int idx);
    void clearCursors();
    void setMeasurementPair(int ref, int target);
    const std::vector<WaveformCursor>& cursors() const { return cursors_; }
    int  activeCursorCount() const;
    int  measRef()    const { return meas_ref_; }
    int  measTarget() const { return meas_target_; }

    // ── Rolling mode ──────────────────────────────────────────────────────────
    /// If no visible channel has an edge on screen, jump to the newest one.
    bool snapToSignal();
    /// Centre the view on the next/previous edge of a visible channel.
    void jumpToEdge(bool forward);
    void setFollowLatest(bool f)   { follow_latest_ = f; update(); }
    bool followLatest()    const   { return follow_latest_; }
    void setTimePerDiv(double ns);
    void resetCaptureTime()        { clearTriggerMark(); capture_start_ms_ = QDateTime::currentMSecsSinceEpoch(); time_offset_ns_ = 0; follow_latest_ = true; }
    void setChannelVisible(int ch, bool v) { if(ch>=0&&ch<8){ch_visible_[ch]=v; update();} }
    bool channelVisible(int ch)    const   { return (ch>=0&&ch<8) ? ch_visible_[ch] : false; }

    /// Measure the period of the first visible channel from its most
    /// recent edges and set T/div so a few cycles fit across the screen.
    /// Used by the toolbar's AUTO button. Returns false if there isn't
    /// enough recent edge data on any visible channel to measure a period.
    bool autoScaleTimeDiv();

    QImage grabScreenshot();

    /// Decoded protocol events drawn on a lane under channel @p channel (S, P, hex
    /// bytes with A / N). @p events must be sorted by time. The widget keeps the
    /// shared list and re-lays it out for the current zoom on every repaint.
    void setAnnotations(int channel, std::shared_ptr<const std::vector<escope::DecodedEvent>> events);
    void clearAnnotations();
    bool hasAnnotations(int channel) const;

    /// Bring [t0, t1] into view (centred, zoomed only when it would not fit or is tiny) and mark it
    /// with a band, so a frame picked in the protocol list is easy to find.
    void revealRange(double t0_ns, double t1_ns);
    void clearHighlight();
    /// Show the moment a trigger fired: the view stops following and glides so @p t_ns sits near the left.
    void showTriggerAt(double t_ns);
    void clearTriggerMark();

signals:
    void cursorsChanged();
    void timeDivChanged(double ns);

protected:
    void initializeGL()                  override;
    void resizeGL(int w, int h)          override;
    void paintGL()                       override;
    void mousePressEvent(QMouseEvent*)   override;
    void mouseMoveEvent(QMouseEvent*)    override;
    void mouseReleaseEvent(QMouseEvent*) override;
    void wheelEvent(QWheelEvent*)        override;
    void keyPressEvent(QKeyEvent*)       override;
    void leaveEvent(QEvent*)             override;

private:
    struct ChannelLayout { float top, bot, mid, px_per_volt; };
    ChannelLayout analogLayout(std::size_t ch)    const;
    float         digitalRowY(std::size_t ch, bool hi) const;
    int           channelAtY(float y)             const;
    int           cursorHitTest(float x, float y) const;

    void buildGridVBO();
    void drawGrid();
    void drawZeroLines();
    void drawAnalogChannel(std::size_t ch);
    void drawDigitalChannel(std::size_t ch);
    void drawOverlay();
    void drawAnnotations(QPainter& p);
    void drawCursors(QPainter& p);
    void drawMeasurementPanel(QPainter& p);

    double pixelToTime(float x)         const;
    float  timeToPixel(double t_ns)     const;
    float  voltToPixel(float v, int ch) const;
    float  pixelToVolt(float y, int ch) const;
    void   clampTimeOffset();
    /// Glide to a new zoom and position instead of jumping (about 180 ms, eased).
    void   animateView(double time_per_div_ns, double offset_ns, int duration_ms = 180,
                       QEasingCurve::Type curve = QEasingCurve::OutCubic);
    double clampedOffset(double offset_ns, double time_per_div_ns) const;

    void   updateCursorVoltages(WaveformCursor& c);

    // Protocol annotation lanes
    struct AnnotationLane { int channel; escope::AnnotationIndex index; };
    struct HitBox { QRectF rect; QString tip; };
    static constexpr float ANNOT_LANE_H = 26.f;
    std::vector<AnnotationLane> lanes_;
    std::vector<HitBox>         hit_boxes_;      ///< filled while painting, used for tooltips
    QPointF                     hover_pos_ {-1, -1};

    // OpenGL
    QOpenGLShaderProgram     shader_;
    QOpenGLVertexArrayObject vao_;
    QOpenGLBuffer  grid_vbo_ {QOpenGLBuffer::VertexBuffer};
    QOpenGLBuffer  wave_vbo_ {QOpenGLBuffer::VertexBuffer};
    int            grid_vertex_count_ = 0;
    static constexpr int MAX_WAVE_VERTS = 32 * 1024;

    // Per-frame reused buffers
    std::vector<escope::AnalogSample> snap_[2];
    // Per-channel pixel-column summaries. They are rebuilt only when the view, the width or the stored
    // edges change, so an idle or paused view costs almost nothing per frame.
    struct ColumnCache {
        double   t0 = 0, t1 = 0;
        int      width = 0;
        uint64_t version = ~0ULL;
        std::vector<escope::DigitalBuffer::EdgeColumn> cols;
    };
    ColumnCache col_cache_[8];
    std::vector<float>                wave_verts_;

    // Animated view changes
    QVariantAnimation* view_anim_ = nullptr;
    double anim_from_tdiv_ = 0, anim_from_off_ = 0, anim_to_tdiv_ = 0, anim_to_off_ = 0;

    double hl_t0_ = 0, hl_t1_ = 0;
    bool   hl_on_ = false;
    double trig_t_ = 0, trig_alpha_ = 0.0;
    bool   trig_on_ = false;
    QVariantAnimation* trig_anim_ = nullptr;
    double hl_alpha_ = 0.0;
    QVariantAnimation* hl_anim_ = nullptr;
    void fadeHighlight(double to);

    // Flick-to-pan: the last few pointer samples of a drag give its release velocity
    struct DragSample { qint64 ms; int x; };
    DragSample drag_samples_[4] = {};
    int        drag_sample_n_ = 0;
    QElapsedTimer drag_clock_;

    // The cursor readout card fades in when it first appears
    QVariantAnimation* card_anim_ = nullptr;
    double card_alpha_ = 0.0;
    bool   card_shown_ = false;

    // View state
    double time_offset_ns_  = 0.0;
    double time_per_div_ns_ = 2e6;
    float  volt_per_div_[2] = {0.5f, 1.0f};
    float  v_offset_[2]     = {0.0f, 0.0f};

    // Cursors
    std::vector<WaveformCursor> cursors_;
    int  meas_ref_    = -1;
    int  meas_target_ = -1;
    int  drag_cursor_ = -1;

    // Pan
    bool   dragging_  = false;
    QPoint drag_start_;
    double drag_t0_   = 0.0;

    bool follow_latest_ = true;
    // Only D0 shown by default -- every additional visible channel means
    // more edges decoded into GL vertices and drawn each frame, so start
    // minimal and let the user opt into more via the "Select" toolbar menu.
    bool ch_visible_[8]  = {true,false,false,false,false,false,false,false};
    /* Rectangle zoom */
    bool   rubber_band_active_ = false;
    QPoint rubber_band_start_;
    QPoint rubber_band_end_;
    qint64 capture_start_ms_ = 0;
    const escope::CaptureSession* session_ = nullptr;
};
