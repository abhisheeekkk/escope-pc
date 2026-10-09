#pragma once
#include <QDialog>
#include <functional>
#include "acquisition/ProtocolTrigger.h"
#include "panels/ProtocolPanel.h"

class QComboBox;
class QLabel;
class QLineEdit;
class QPushButton;
class QStackedWidget;

/// Sheet for choosing what the protocol trigger waits for. The protocol and its pins come from the
/// protocol panel; this only asks for the frame to wait for.
class TriggerDialog : public QDialog {
    Q_OBJECT
public:
    explicit TriggerDialog(QWidget* parent = nullptr);

    /// Show the form for the current protocol. @p armed is the config in force (Kind::Off if none).
    void present(const ProtocolPanel::PinSetup& pins, const escope::ProtocolTriggerConfig& armed);

signals:
    void armRequested(escope::ProtocolTriggerConfig cfg);
    void disarmRequested();

protected:
    void showEvent(QShowEvent* e) override;

private:
    escope::ProtocolTriggerConfig build() const;
    void updateButtons();

    ProtocolPanel::PinSetup pins_;
    bool armed_ = false;

    QLabel*         hint_    = nullptr;
    QStackedWidget* pages_   = nullptr;
    QComboBox*      what_    = nullptr;     // a specific frame / any error
    // I2C
    QLineEdit* i2c_addr_ = nullptr; QComboBox* i2c_dir_ = nullptr; QComboBox* i2c_ack_ = nullptr;
    // UART
    QLineEdit* uart_byte_ = nullptr; QComboBox* uart_baud_ = nullptr;
    // CAN
    QLineEdit* can_id_ = nullptr; QComboBox* can_fmt_ = nullptr;
    // SPI
    QComboBox* spi_line_ = nullptr; QLineEdit* spi_byte_ = nullptr;
    QLabel*      note_   = nullptr;
    QPushButton* arm_    = nullptr;
    QPushButton* off_    = nullptr;
};
