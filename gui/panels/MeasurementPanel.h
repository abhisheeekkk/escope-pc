#pragma once
#include <QWidget>
#include "session/CaptureSession.h"

class QLabel;
class QGridLayout;

/// Digital-only measurement panel.
/// Shows per-channel: frequency, period, duty cycle, pulse width (hi and lo).
class MeasurementPanel : public QWidget {
    Q_OBJECT
public:
    explicit MeasurementPanel(QWidget* parent = nullptr);
    void updateFrom(const escope::CaptureSession& session);
    void setFocusChannel(int ch);  // highlight measurements for this channel
private:
    int focus_ch_ = 0;
    // Rows for the focused channel
    QLabel *lbl_ch_, *lbl_freq_, *lbl_period_,
           *lbl_duty_, *lbl_pw_hi_, *lbl_pw_lo_,
           *lbl_edges_, *lbl_level_;
};
