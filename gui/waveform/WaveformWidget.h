#pragma once

#include <QOpenGLWidget>
#include <QOpenGLFunctions>
#include <QOpenGLShaderProgram>
#include <QOpenGLBuffer>
#include <QOpenGLVertexArrayObject>
#include <QPainter>
#include <QImage>
#include <QColor>

#include "acquisition/SampleBuffer.h"
#include "acquisition/DigitalBuffer.h"

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

    static QColor color(int idx) {
        static const QColor pal[8] = {
            {255,200, 50}, {50,200,255}, {255,80,180}, {80,255,120},
            {255,120, 50}, {160,80,255}, {255,255,255}, {255,50,50}
        };
        return pal[idx % 8];
    }
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
    void removeCursor(int idx);
    void clearCursors();
    void setMeasurementPair(int ref, int target);
    const std::vector<WaveformCursor>& cursors() const { return cursors_; }
    int  activeCursorCount() const;
    int  measRef()    const { return meas_ref_; }
    int  measTarget() const { return meas_target_; }

    // ── Rolling mode ──────────────────────────────────────────────────────────
    void setFollowLatest(bool f) { follow_latest_ = f; }
    bool followLatest()    const { return follow_latest_; }

    QImage grabScreenshot();

signals:
    void cursorsChanged();

protected:
    void initializeGL()                  override;
    void resizeGL(int w, int h)          override;
    void paintGL()                       override;
    void mousePressEvent(QMouseEvent*)   override;
    void mouseMoveEvent(QMouseEvent*)    override;
    void mouseReleaseEvent(QMouseEvent*) override;
    void wheelEvent(QWheelEvent*)        override;
    void keyPressEvent(QKeyEvent*)       override;

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
    void drawCursors(QPainter& p);
    void drawMeasurementPanel(QPainter& p);

    double pixelToTime(float x)         const;
    float  timeToPixel(double t_ns)     const;
    float  voltToPixel(float v, int ch) const;
    float  pixelToVolt(float y, int ch) const;

    void   updateCursorVoltages(WaveformCursor& c);

    // OpenGL
    QOpenGLShaderProgram     shader_;
    QOpenGLVertexArrayObject vao_;
    QOpenGLBuffer  grid_vbo_ {QOpenGLBuffer::VertexBuffer};
    QOpenGLBuffer  wave_vbo_ {QOpenGLBuffer::VertexBuffer};
    int            grid_vertex_count_ = 0;
    static constexpr int MAX_WAVE_VERTS = 16 * 1024;

    // Per-frame reused buffers
    std::vector<escope::AnalogSample> snap_[2];
    std::vector<escope::DigitalEdge>  dig_snap_;
    std::vector<float>                wave_verts_;

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
    const escope::CaptureSession* session_ = nullptr;
};
