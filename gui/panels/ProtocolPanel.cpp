#include "panels/ProtocolPanel.h"
#include "session/CaptureSession.h"
#include "decoders/uart/UartDecoder.h"

#include <QComboBox>
#include <QFormLayout>
#include <QLabel>
#include <QPlainTextEdit>
#include <QScrollBar>
#include <QStandardItemModel>
#include <QVBoxLayout>
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

    title_ = new QLabel(this);
    title_->setStyleSheet("color:#00E666; font-weight:bold;");
    root->addWidget(title_);

    auto* form = new QFormLayout;
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
    output_->clear();
    summary_->clear();
}

void ProtocolPanel::updateFrom(const escope::CaptureSession& session,
                               double view_t0_ns, double view_t1_ns) {
    if (protocol_.isEmpty()) return;

    struct Line { int ch; const char* role; };
    std::vector<Line> lines_to_decode;
    if (tx_->currentData().toInt() >= 0) lines_to_decode.push_back({tx_->currentData().toInt(), "TX"});
    if (rx_->currentData().toInt() >= 0) lines_to_decode.push_back({rx_->currentData().toInt(), "RX"});
    if (lines_to_decode.empty()) {
        summary_->setText("Select a TX or RX pin");
        baud_->setText("Baud: auto");
        output_->clear();
        return;
    }

    const auto& buf = session.digital_buffer();
    std::vector<escope::DecodedEvent> events;
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
    summary_->setText(QString("%1 frames, %2 errors").arg(events.size()).arg(errors));
}
