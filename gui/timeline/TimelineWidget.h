#pragma once
#include <QWidget>
namespace escope { class CaptureSession; }

/// Unified timeline showing decoded protocol events alongside analog/digital channels.
/// Shares the same time axis as WaveformWidget (synchronized via shared view state).
class TimelineWidget : public QWidget {
    Q_OBJECT
public:
    explicit TimelineWidget(QWidget* parent = nullptr);
    void setSession(const escope::CaptureSession* session);
protected:
    void paintEvent(QPaintEvent*) override;
private:
    const escope::CaptureSession* session_ = nullptr;
};
