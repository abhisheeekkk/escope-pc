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

    /// Hide everything decoded so far (new frames still appear).
    void clearDecoded();

private:
    void fillChannelCombo(QComboBox* c, int select);

    const escope::CaptureSession* session_ = nullptr;  ///< last session seen by updateFrom
    double          clear_before_ns_ = -1.0;           ///< frames starting before this are hidden
    QString         protocol_;
    QLabel*         title_   = nullptr;
    QComboBox*      tx_      = nullptr;
    QComboBox*      rx_      = nullptr;
    QLabel*         baud_    = nullptr;
    QLabel*         summary_ = nullptr;
    QPlainTextEdit* text_    = nullptr;   ///< full decoded byte stream as text
    QPlainTextEdit* output_  = nullptr;
};
