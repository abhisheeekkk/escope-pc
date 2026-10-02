#pragma once
#include <QWidget>
#include <QString>

namespace escope { class CaptureSession; }
class QComboBox;
class QPlainTextEdit;
class QLabel;

/// Protocol decoder panel. Hidden until a protocol is chosen from the
/// toolbar's "Protocol" button; then shows the TX/RX pin pickers and the
/// decoded frames. The baud rate is detected from the signal.
class ProtocolPanel : public QWidget {
    Q_OBJECT
public:
    explicit ProtocolPanel(QWidget* parent = nullptr);

    /// Select the active protocol ("UART"), or an empty string for off.
    void setProtocol(const QString& name);
    bool active() const { return !protocol_.isEmpty(); }

    /// Re-decode the frames inside the waveform's visible window.
    void updateFrom(const escope::CaptureSession& session,
                    double view_t0_ns, double view_t1_ns);

private:
    void fillChannelCombo(QComboBox* c, int select);

    QString         protocol_;
    QLabel*         title_   = nullptr;
    QComboBox*      tx_      = nullptr;
    QComboBox*      rx_      = nullptr;
    QLabel*         baud_    = nullptr;
    QLabel*         summary_ = nullptr;
    QPlainTextEdit* output_  = nullptr;
};
