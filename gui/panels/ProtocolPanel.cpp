#include "panels/ProtocolPanel.h"
#include "session/CaptureSession.h"
#include "decoders/uart/UartDecoder.h"
#include "decoders/i2c/I2CDecoder.h"

#include <QComboBox>
#include <QFormLayout>
#include <QLabel>
#include <QClipboard>
#include <QGuiApplication>
#include <QPushButton>
#include <QTimer>
#include <QPlainTextEdit>
#include <QScrollBar>
#include <QStandardItemModel>
#include <QVBoxLayout>
#include <QHBoxLayout>
#include <algorithm>

namespace {
constexpr int MAX_LINES = 500;
constexpr int NUM_CH    = 8;

QString formatTime(double ns) {
    if (ns >= 1e9) return QString::number(ns / 1e9, 'f', 6) + " s";
    if (ns >= 1e6) return QString::number(ns / 1e6, 'f', 4) + " ms";
    return QString::number(ns / 1e3, 'f', 2) + " us";
}
}

ProtocolPanel::ProtocolPanel(QWidget* parent) : QWidget(parent) {
    auto* root = new QVBoxLayout(this);
    root->setContentsMargins(4, 4, 4, 4);

    auto* head = new QHBoxLayout;
    title_ = new QLabel(this);
    title_->setStyleSheet("color:#00E666; font-weight:bold;");
    head->addWidget(title_, 1);
    auto* clear_btn = new QPushButton("Clear", this);
    connect(clear_btn, &QPushButton::clicked, this, &ProtocolPanel::clearDecoded);
    head->addWidget(clear_btn);
    // Copies the whole decoded text / hex log box (not just the selection).
    auto* copy_btn = new QPushButton("Copy", this);
    connect(copy_btn, &QPushButton::clicked, this, [this, copy_btn]() {
        QGuiApplication::clipboard()->setText(text_->toPlainText());
        copy_btn->setText("Copied!");
        QTimer::singleShot(1200, copy_btn, [copy_btn]{ copy_btn->setText("Copy"); });
    });
    head->addWidget(copy_btn);
    root->addLayout(head);

    auto* form = new QFormLayout;
    form_ = form;
    form->setContentsMargins(0, 0, 0, 0);
    tx_ = new QComboBox(this);
    rx_ = new QComboBox(this);
    fillChannelCombo(tx_, 0);    // D0
    fillChannelCombo(rx_, -1);   // none
    // A pin can only be TX or RX, never both: grey out the other combo's choice.
    auto exclude = [](QComboBox* chosen, QComboBox* other) {
        auto* model = qobject_cast<QStandardItemModel*>(other->model());
        const int ch = chosen->currentData().toInt();
        for (int i = 1; i < other->count(); ++i) {       // item 0 is "None"
            auto* item = model->item(i);
            const bool taken = other->itemData(i).toInt() == ch;
            item->setFlags(taken ? item->flags() & ~(Qt::ItemIsEnabled | Qt::ItemIsSelectable)
                                 : item->flags() | Qt::ItemIsEnabled | Qt::ItemIsSelectable);
        }
    };
    connect(tx_, &QComboBox::currentIndexChanged, this, [=]{ exclude(tx_, rx_); });
    connect(rx_, &QComboBox::currentIndexChanged, this, [=]{ exclude(rx_, tx_); });
    exclude(tx_, rx_);
    exclude(rx_, tx_);
    form->addRow("TX pin:", tx_);
    form->addRow("RX pin:", rx_);
    baud_ = new QLabel("Baud: auto", this);
    baud_->setStyleSheet("color:#aaa;");
    form->addRow(baud_);
    root->addLayout(form);

    summary_ = new QLabel(this);
    summary_->setStyleSheet("color:#888; font-size:10px;");
    root->addWidget(summary_);

    text_ = new QPlainTextEdit(this);
    text_->setReadOnly(true);
    text_->setPlaceholderText("Decoded text (whole capture)");
    text_->setStyleSheet("background:#111; color:#00E666; font-family:monospace; font-size:11px;");
    root->addWidget(text_, 1);

    output_ = new QPlainTextEdit(this);
    output_->setReadOnly(true);
    output_->setLineWrapMode(QPlainTextEdit::NoWrap);
    output_->setStyleSheet("background:#111; color:#ddd; font-family:monospace; font-size:11px;");
    root->addWidget(output_, 1);

    setMinimumWidth(220);
}

void ProtocolPanel::fillChannelCombo(QComboBox* c, int select) {
    c->addItem("None", -1);
    for (int i = 0; i < NUM_CH; ++i) c->addItem(QString("D%1").arg(i), i);
    c->setCurrentIndex(select + 1);
}

void ProtocolPanel::setProtocol(const QString& name) {
    protocol_ = name;
    title_->setText(name.isEmpty() ? QString() : name + " decoder");
    // The two pin pickers double as TX/RX (UART) and SDA/SCL (I2C).
    const bool i2c = (name == "I2C");
    if (auto* l = qobject_cast<QLabel*>(form_->labelForField(tx_))) l->setText(i2c ? "SDA pin:" : "TX pin:");
    if (auto* l = qobject_cast<QLabel*>(form_->labelForField(rx_))) l->setText(i2c ? "SCL pin:" : "RX pin:");
    if (!name.isEmpty()) {
        // Defaults follow the board's labelling: UART0 on D0; I2C SDA=D6, SCL=D5.
        const int a = i2c ? 6 : 0, b = i2c ? 5 : -1;
        rx_->setCurrentIndex(0);                 // free both before assigning
        tx_->setCurrentIndex(a + 1);
        rx_->setCurrentIndex(b + 1);
    }
    text_->setPlaceholderText(i2c ? "Hex log (whole capture)" : "Decoded text (whole capture)");
    clear_before_ns_ = -1.0;
    output_->clear();
    text_->clear();
    summary_->clear();
}

void ProtocolPanel::clearDecoded() {
    // The decoder re-decodes the whole history each refresh (to stay locked to
    // real start bits), so "clear" = ignore frames up to the newest edge now.
    if (session_) clear_before_ns_ = session_->digital_buffer().time_range_ns().second;
    text_->clear();
    output_->clear();
    summary_->clear();
}

void ProtocolPanel::updateFrom(const escope::CaptureSession& session,
                               double view_t0_ns, double view_t1_ns) {
    session_ = &session;
    if (protocol_.isEmpty()) return;

    const auto& buf = session.digital_buffer();
    std::vector<escope::DecodedEvent> events;
    const bool i2c = (protocol_ == "I2C");
    std::size_t glitches = 0;      // short pulses the decoder ignored (I2C)

    if (i2c) {
        const int sda = tx_->currentData().toInt();
        const int scl = rx_->currentData().toInt();
        if (sda < 0 || scl < 0) {
            summary_->setText("Select SDA and SCL pins");
            baud_->setText("Speed: --");
            output_->clear();
            text_->clear();
            return;
        }
        escope::I2CDecoder dec;
        // Decode the full history; only the visible window is listed below.
        events = dec.decode(buf, {{"SCL", static_cast<uint8_t>(scl)},
                                  {"SDA", static_cast<uint8_t>(sda)}});
        glitches = dec.glitches_filtered();

        // Bus speed from the median SCL period of the most recent edges.
        std::vector<double> periods;
        double prev = -1;
        for (const auto& e : buf.last_edges(static_cast<uint8_t>(scl), 400)) {
            if (!e.rising) continue;
            if (prev > 0) periods.push_back(e.timestamp_ns - prev);
            prev = e.timestamp_ns;
        }
        if (periods.empty()) {
            baud_->setText("Speed: no clock");
        } else {
            std::sort(periods.begin(), periods.end());
            baud_->setText(QString("SCL ~%1 kHz").arg(1e6 / periods[periods.size() / 2], 0, 'f', 1));
        }
    } else {
        struct Line { int ch; const char* role; };
        std::vector<Line> lines_to_decode;
        if (tx_->currentData().toInt() >= 0) lines_to_decode.push_back({tx_->currentData().toInt(), "TX"});
        if (rx_->currentData().toInt() >= 0) lines_to_decode.push_back({rx_->currentData().toInt(), "RX"});
        if (lines_to_decode.empty()) {
            summary_->setText("Select a TX or RX pin");
            baud_->setText("Baud: auto");
            output_->clear();
            text_->clear();
            return;
        }

        QStringList baud_text;
        // TX and RX are detected separately so they don't need to match.
        for (const auto& l : lines_to_decode) {
            const uint32_t baud = escope::UartDecoder::detect_baud(buf, static_cast<uint8_t>(l.ch));
            if (!baud) { baud_text << QString("%1: no signal").arg(l.role); continue; }
            baud_text << QString("%1: %2").arg(l.role).arg(baud);

            escope::UartDecoder dec;
            dec.configure({{"baud", std::to_string(baud)}});
            // Decode the full history so framing stays locked to real start bits;
            // only the visible window is listed below.
            auto ev = dec.decode(buf, {{l.role, static_cast<uint8_t>(l.ch)}});
            events.insert(events.end(), ev.begin(), ev.end());
        }
        baud_->setText("Baud (auto): " + baud_text.join("   "));
    }

    events.erase(std::remove_if(events.begin(), events.end(),
        [&](const escope::DecodedEvent& e) { return e.start_ns <= clear_before_ns_; }),
        events.end());

    // Rebuild the whole decoded stream as text, independent of the visible
    // window, so a long string isn't cut off by zoom/scroll. Per line (TX/RX).
    if (i2c) {
        std::sort(events.begin(), events.end(),
            [](const auto& a, const auto& b) { return a.start_ns < b.start_ns; });
        // Hex log of every transaction in the capture (not window-limited):
        //   <time>  S [50 W] A  12 A  34 N  P      (A = ACK, N = NACK)
        //   <time>  ~ 12 A 34 A ...                (burst began mid-transfer;
        //                                           byte alignment inferred)
        QStringList lines;
        QString cur;
        auto flush = [&] { if (!cur.isEmpty()) lines << cur; cur.clear(); };
        auto hex = [](int v) { return QString("%1").arg(v & 0xFF, 2, 16, QChar('0')).toUpper(); };
        for (const auto& e : events) {
            using T = escope::DecodedEvent::Type;
            const QString lbl = QString::fromStdString(e.label);
            const QString ack = lbl.endsWith(" NACK") ? "N" : "A";
            if (lbl == "START") { flush(); cur = QString("%1  S").arg(formatTime(e.start_ns), 12); }
            else if (lbl == "~") { flush(); cur = QString("%1  ~").arg(formatTime(e.start_ns), 12); }
            else if (lbl == "Sr") cur += " Sr";
            else if (lbl == "STOP") { cur += " P"; flush(); }
            else if (e.type == T::Address)
                cur += QString(" [%1 %2] %3").arg(hex(e.value >> 1), (e.value & 1) ? "R" : "W", ack);
            else if (e.type == T::Data) cur += QString(" %1 %2").arg(hex(e.value), ack);
            else if (e.is_error) {
                if (cur.isEmpty()) cur = QString("%1 ").arg(formatTime(e.start_ns), 12);
                cur += " !" + lbl;
            }
        }
        flush();
        constexpr int MAX_LINES_LOG = 5000;   // keep the log bounded, newest kept
        if (lines.size() > MAX_LINES_LOG) lines = lines.mid(lines.size() - MAX_LINES_LOG);
        text_->setPlainText(lines.join('\n'));
        text_->verticalScrollBar()->setValue(text_->verticalScrollBar()->maximum());
    } else {
        std::sort(events.begin(), events.end(),
            [](const auto& a, const auto& b) { return a.start_ns < b.start_ns; });
        QString tx_text, rx_text;
        for (const auto& e : events) {
            if (e.is_error || e.value < 0) continue;
            QString& dst = (e.label.compare(0, 2, "TX") == 0) ? tx_text : rx_text;
            const char c = static_cast<char>(e.value);
            if (c == '\n') dst += '\n';
            else if (c == '\r') continue;
            else dst += (c >= 32 && c < 127) ? QChar(c) : QChar('.');
        }
        constexpr int MAX_CHARS = 20000;
        auto tail = [](QString s) { return s.size() > MAX_CHARS ? s.right(MAX_CHARS) : s; };
        QString out;
        if (!tx_text.isEmpty()) out += "TX: " + tail(tx_text);
        if (!rx_text.isEmpty()) out += (out.isEmpty() ? "" : "\n") + QString("RX: ") + tail(rx_text);
        text_->setPlainText(out);
        text_->verticalScrollBar()->setValue(text_->verticalScrollBar()->maximum());
    }

    events.erase(std::remove_if(events.begin(), events.end(),
        [&](const escope::DecodedEvent& e) {
            return e.end_ns < view_t0_ns || e.start_ns > view_t1_ns;
        }), events.end());
    std::sort(events.begin(), events.end(),
        [](const auto& a, const auto& b) { return a.start_ns < b.start_ns; });

    int errors = 0;
    for (const auto& e : events) errors += e.is_error;
    const std::size_t first = events.size() > MAX_LINES ? events.size() - MAX_LINES : 0;

    QStringList text;
    for (std::size_t i = first; i < events.size(); ++i)
        text << QString("%1  %2").arg(formatTime(events[i].start_ns), 12)
                                 .arg(QString::fromStdString(events[i].label));
    output_->setPlainText(text.join('\n'));
    output_->verticalScrollBar()->setValue(output_->verticalScrollBar()->maximum());
    QString sum = QString("%1 frames, %2 errors").arg(events.size()).arg(errors);
    if (i2c && glitches) sum += QString(", %1 glitches ignored").arg(glitches);
    summary_->setText(sum);
}
