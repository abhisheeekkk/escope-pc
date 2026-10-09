#pragma once
#include <QWidget>
#include <QString>
#include <QVector>
#include <memory>
#include <vector>
#include "decoders/base/IDecoder.h"

namespace escope { class CaptureSession; }
class QComboBox;
class QPlainTextEdit;
class QLabel;
class QFormLayout;

/// Protocol decoder panel. Hidden until a protocol is chosen from the
/// toolbar's "Protocol" button; then shows the TX/RX pin pickers and the
/// decoded frames. The baud rate is detected from the signal.
class ProtocolPanel : public QWidget {
    Q_OBJECT
public:
    explicit ProtocolPanel(QWidget* parent = nullptr);

    /// Select the active protocol ("UART", "I2C", "CAN" or "SPI"), or an empty string for off.
    void setProtocol(const QString& name);
    bool active() const { return !protocol_.isEmpty(); }

    /// Re-decode the frames inside the waveform's visible window.
    void updateFrom(const escope::CaptureSession& session,
                    double view_t0_ns, double view_t1_ns);

    /// Hide everything decoded so far (new frames still appear).
    void clearDecoded();

    /// True while the capture is running and the view follows the newest data. Then only the
    /// last couple of seconds are decoded, so the decode keeps up with the incoming bursts;
    /// when the capture stops or the view is paused the whole history is decoded once.
    void setLive(bool live) { live_ = live; }

signals:
    /// The full decoded stream for one channel (sorted by time), for the waveform
    /// to draw as S / P / hex / A / N annotations under that channel.
    void annotationsChanged(int channel,
                            std::shared_ptr<const std::vector<escope::DecodedEvent>> events);
    /// The decoder was switched off or its pins changed: remove the annotations.
    void annotationsCleared();
    /// Channels the chosen protocol uses (SDA and SCL, or TX / RX), so the main
    /// view can show them.
    void pinsChanged(QVector<int> channels);

private:
    void fillChannelCombo(QComboBox* c, int select);
    /// The pin pickers the current protocol uses (two, or three for SPI).
    std::vector<QComboBox*> activePins() const;
    void refreshExclusion();
    void setPinRow(QComboBox* c, const QString& label, bool visible);

    void emitPins();
    void dropAnnotations();
    bool                 live_ = false;
    bool                 annot_active_ = false;   ///< annotations are currently on the waveform
    std::vector<double>  last_sig_;               ///< data inputs of the last decode, to skip an identical one
    bool                 cache_valid_ = false;    ///< cache_events_ matches last_sig_
    std::vector<escope::DecodedEvent> cache_events_;   ///< last decode (all events), re-listed when only the view moves
    std::size_t          cache_glitches_ = 0;
    double               last_view0_ = 0, last_view1_ = 0;
    const escope::CaptureSession* session_ = nullptr;  ///< last session seen by updateFrom
    double          clear_before_ns_ = -1.0;           ///< frames starting before this are hidden
    QFormLayout*    form_    = nullptr;
    QString         protocol_;
    QLabel*         title_   = nullptr;
    QComboBox*      tx_      = nullptr;
    QComboBox*      rx_      = nullptr;
    QComboBox*      p3_      = nullptr;   ///< third pin (SPI: MISO)
    QLabel*         baud_    = nullptr;
    QLabel*         summary_ = nullptr;
    QPlainTextEdit* text_    = nullptr;   ///< full decoded byte stream as text
    QPlainTextEdit* output_  = nullptr;
};
