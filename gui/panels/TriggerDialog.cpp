#include "panels/TriggerDialog.h"
#include "theme/Theme.h"
#include "theme/Motion.h"

#include <QComboBox>
#include <QFormLayout>
#include <QHBoxLayout>
#include <QLabel>
#include <QLineEdit>
#include <QPushButton>
#include <QRegularExpressionValidator>
#include <QStackedWidget>
#include <QVBoxLayout>

using escope::ProtocolTriggerConfig;
using Kind = ProtocolTriggerConfig::Kind;

namespace {
QLineEdit* hexEdit(QWidget* parent, int max_digits, const QString& placeholder) {
    auto* e = new QLineEdit(parent);
    e->setPlaceholderText(placeholder);
    e->setValidator(new QRegularExpressionValidator(
        QRegularExpression(QString("(0[xX])?[0-9a-fA-F]{0,%1}").arg(max_digits)), e));
    e->setFont(theme::mono(10.0));
    return e;
}
bool parseHex(const QLineEdit* e, uint32_t& out) {
    QString t = e->text().trimmed();
    if (t.startsWith("0x", Qt::CaseInsensitive)) t = t.mid(2);
    bool ok = false;
    out = t.toUInt(&ok, 16);
    return ok && !t.isEmpty();
}
}

TriggerDialog::TriggerDialog(QWidget* parent) : QDialog(parent) {
    setWindowTitle("Protocol Trigger");
    setModal(false);
    setMinimumWidth(380);
    auto* root = new QVBoxLayout(this);
    root->setContentsMargins(24, 22, 24, 20);
    root->setSpacing(14);

    auto* title = new QLabel("Protocol trigger", this);
    title->setFont(theme::ui(13.5, QFont::DemiBold));
    root->addWidget(title);

    hint_ = new QLabel(this);
    hint_->setWordWrap(true);
    hint_->setObjectName("hint");
    root->addWidget(hint_);

    auto* form = new QFormLayout;
    form->setHorizontalSpacing(18);
    form->setVerticalSpacing(10);
    what_ = new QComboBox(this);
    what_->addItems({"A specific frame", "Any error"});
    form->addRow("Wait for", what_);
    root->addLayout(form);

    pages_ = new QStackedWidget(this);
    auto page = [&](std::function<void(QFormLayout*)> fill) {
        auto* w = new QWidget(pages_);
        auto* f = new QFormLayout(w);
        f->setContentsMargins(0, 0, 0, 0);
        f->setHorizontalSpacing(18);
        f->setVerticalSpacing(10);
        fill(f);
        pages_->addWidget(w);
    };
    page([&](QFormLayout* f) {                       // 0: I2C
        i2c_addr_ = hexEdit(this, 2, "50");
        i2c_dir_ = new QComboBox(this); i2c_dir_->addItems({"Read or write", "Write", "Read"});
        i2c_ack_ = new QComboBox(this); i2c_ack_->addItems({"ACK or NACK", "ACK", "NACK"});
        f->addRow("7-bit address (hex)", i2c_addr_);
        f->addRow("Direction", i2c_dir_);
        f->addRow("Reply", i2c_ack_);
    });
    page([&](QFormLayout* f) {                       // 1: UART
        uart_byte_ = hexEdit(this, 2, "0A");
        uart_baud_ = new QComboBox(this);
        for (int b : {1200, 2400, 4800, 9600, 19200, 38400, 57600, 115200, 230400, 460800, 921600, 1000000, 2000000})
            uart_baud_->addItem(QString::number(b), b);
        uart_baud_->setCurrentIndex(uart_baud_->findData(115200));
        f->addRow("Byte (hex)", uart_byte_);
        f->addRow("Baud rate", uart_baud_);
    });
    page([&](QFormLayout* f) {                       // 2: CAN
        can_id_ = hexEdit(this, 8, "123");
        can_fmt_ = new QComboBox(this); can_fmt_->addItems({"Standard (11 bit)", "Extended (29 bit)"});
        f->addRow("Identifier (hex)", can_id_);
        f->addRow("Format", can_fmt_);
    });
    page([&](QFormLayout* f) {                       // 3: SPI
        spi_line_ = new QComboBox(this);
        spi_byte_ = hexEdit(this, 2, "9F");
        f->addRow("Data line", spi_line_);
        f->addRow("Word (hex)", spi_byte_);
    });
    page([&](QFormLayout*) {});                      // 4: nothing more to ask (any error)
    root->addWidget(pages_);

    note_ = new QLabel(this);
    note_->setWordWrap(true);
    note_->setObjectName("caption");
    note_->setText("The capture runs live and keeps updating. When the frame appears, that capture is "
                   "kept and the capture stops, lined up on the frame. Press Run to arm it again. A frame "
                   "that falls in the short gap between two captures can be missed.");
    root->addWidget(note_);

    auto* row = new QHBoxLayout;
    row->addStretch();
    off_ = new QPushButton("Turn off", this);
    arm_ = new QPushButton("Arm trigger", this);
    arm_->setDefault(true);
    row->addWidget(off_);
    row->addWidget(arm_);
    root->addLayout(row);

    auto sync = [this] {
        const bool any_error = what_->currentIndex() == 1;
        pages_->setCurrentIndex(any_error ? 4 : pages_->property("proto_page").toInt());
        updateButtons();
    };
    connect(what_, &QComboBox::currentIndexChanged, this, sync);
    for (QLineEdit* e : {i2c_addr_, uart_byte_, can_id_, spi_byte_})
        connect(e, &QLineEdit::textChanged, this, [this] { updateButtons(); });
    connect(arm_, &QPushButton::clicked, this, [this] { emit armRequested(build()); });
    connect(off_, &QPushButton::clicked, this, [this] { emit disarmRequested(); });
}

void TriggerDialog::present(const ProtocolPanel::PinSetup& pins, const ProtocolTriggerConfig& armed) {
    pins_ = pins;
    armed_ = armed.kind != Kind::Off;
    const bool ok = pins.complete();
    hint_->setText(ok ? QString("Capture until a %1 frame appears, then stop on it.").arg(pins.protocol)
                      : "Choose a protocol and its pins first (toolbar > Protocol).");
    for (QWidget* w : {static_cast<QWidget*>(what_), static_cast<QWidget*>(pages_)}) w->setEnabled(ok);

    int page = 0;
    if (pins.protocol == "UART") page = 1; else if (pins.protocol == "CAN") page = 2; else if (pins.protocol == "SPI") page = 3;
    pages_->setProperty("proto_page", page);

    spi_line_->clear();
    for (const auto& c : pins.channels)
        if (c.role == "MOSI" || c.role == "MISO") spi_line_->addItem(QString::fromStdString(c.role));

    if (armed_ && armed.protocol == pins.protocol.toStdString()) {       // show what is armed
        what_->setCurrentIndex(armed.kind == Kind::AnyError ? 1 : 0);
        auto hex = [](uint32_t v) { return QString::number(v, 16).toUpper(); };
        switch (armed.kind) {
        case Kind::I2CAddress: i2c_addr_->setText(hex(armed.value));
            i2c_dir_->setCurrentIndex(int(armed.direction)); i2c_ack_->setCurrentIndex(int(armed.ack)); break;
        case Kind::UartByte:   uart_byte_->setText(hex(armed.value)); break;
        case Kind::CanId:      can_id_->setText(hex(armed.value)); can_fmt_->setCurrentIndex(armed.extended ? 1 : 0); break;
        case Kind::SpiByte:    spi_byte_->setText(hex(armed.value));
            spi_line_->setCurrentText(QString::fromStdString(armed.spi_line)); break;
        default: break;
        }
    }
    pages_->setCurrentIndex(what_->currentIndex() == 1 ? 4 : page);
    updateButtons();
}

ProtocolTriggerConfig TriggerDialog::build() const {
    ProtocolTriggerConfig c;
    c.protocol = pins_.protocol.toStdString();
    c.channels = pins_.channels;
    uint32_t v = 0;
    if (what_->currentIndex() == 1) { c.kind = Kind::AnyError; return c; }
    if (c.protocol == "I2C") {
        c.kind = Kind::I2CAddress;
        if (parseHex(i2c_addr_, v)) c.value = v & 0x7F;
        c.direction = ProtocolTriggerConfig::Direction(i2c_dir_->currentIndex());
        c.ack = ProtocolTriggerConfig::Ack(i2c_ack_->currentIndex());
    } else if (c.protocol == "UART") {
        c.kind = Kind::UartByte;
        if (parseHex(uart_byte_, v)) c.value = v & 0xFF;
        c.params = {{"baud", std::to_string(uart_baud_->currentData().toInt())}};
    } else if (c.protocol == "CAN") {
        c.kind = Kind::CanId;
        c.extended = can_fmt_->currentIndex() == 1;
        if (parseHex(can_id_, v)) c.value = c.extended ? (v & 0x1FFFFFFF) : (v & 0x7FF);
    } else {
        c.kind = Kind::SpiByte;
        if (parseHex(spi_byte_, v)) c.value = v & 0xFF;
        c.spi_line = spi_line_->currentText().toStdString();
        c.params = {{"mode", "auto"}};
    }
    return c;
}

void TriggerDialog::updateButtons() {
    uint32_t v;
    bool valid = pins_.complete();
    if (valid && what_->currentIndex() == 0) {
        const QString& p = pins_.protocol;
        valid = p == "I2C" ? parseHex(i2c_addr_, v) : p == "UART" ? parseHex(uart_byte_, v)
              : p == "CAN" ? parseHex(can_id_, v) : (parseHex(spi_byte_, v) && spi_line_->count() > 0);
    }
    arm_->setEnabled(valid);
    arm_->setText(armed_ ? "Update trigger" : "Arm trigger");
    off_->setVisible(armed_);
}

void TriggerDialog::showEvent(QShowEvent* e) {
    QDialog::showEvent(e);
    motion::fadeIn(this, 200);
}
