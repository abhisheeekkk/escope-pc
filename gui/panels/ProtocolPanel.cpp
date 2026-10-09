#include "panels/ProtocolPanel.h"
#include "theme/Theme.h"
#include "panels/FrameListWidget.h"
#include "session/CaptureSession.h"
#include "decoders/uart/UartDecoder.h"
#include "decoders/i2c/I2CDecoder.h"
#include "decoders/can/CANDecoder.h"
#include "decoders/spi/SPIDecoder.h"

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
#include <map>

namespace {
constexpr int MAX_LINES = 500;
constexpr int NUM_CH    = 8;
constexpr double LIVE_WINDOW_NS = 2.0e9;   // decode this much recent data while the capture runs

QString formatTime(double ns) {
    if (ns >= 1e9) return QString::number(ns / 1e9, 'f', 6) + " s";
    if (ns >= 1e6) return QString::number(ns / 1e6, 'f', 4) + " ms";
    return QString::number(ns / 1e3, 'f', 2) + " us";
}
}

ProtocolPanel::ProtocolPanel(QWidget* parent) : QWidget(parent) {
    auto* root = new QVBoxLayout(this);
    root->setContentsMargins(14, 4, 14, 14);
    root->setSpacing(10);

    auto* head = new QHBoxLayout;
    title_ = new QLabel(this);
    title_->setObjectName("sectionTitle");
    title_->setStyleSheet(QString("color:%1;").arg(theme::css(theme::kAccent)));
    head->addWidget(title_, 1);
    auto* clear_btn = new QPushButton("Clear", this);
    connect(clear_btn, &QPushButton::clicked, this, &ProtocolPanel::clearDecoded);
    head->addWidget(clear_btn);
    // Copies the whole decoded text / hex log box (not just the selection).
    auto* copy_btn = new QPushButton("Copy", this);
    connect(copy_btn, &QPushButton::clicked, this, [this, copy_btn]() {
        QGuiApplication::clipboard()->setText(text_->toPlainText());
        copy_btn->setText("Copied");
        QTimer::singleShot(1200, copy_btn, [copy_btn]{ copy_btn->setText("Copy"); });
    });
    head->addWidget(copy_btn);
    root->addLayout(head);

    auto* form = new QFormLayout;
    form_ = form;
    form->setContentsMargins(0, 0, 0, 0);
    form->setVerticalSpacing(8);
    tx_ = new QComboBox(this);
    rx_ = new QComboBox(this);
    p3_ = new QComboBox(this);
    fillChannelCombo(tx_, 0);    // D0
    fillChannelCombo(rx_, -1);   // none
    fillChannelCombo(p3_, -1);
    // Changing a pin invalidates the annotations; show the chosen channels right away.
    auto pinsMoved = [this]{ refreshExclusion(); last_sig_.clear(); cache_valid_ = false; dropAnnotations(); emitPins(); emitRoles(); };
    for (QComboBox* c : {tx_, rx_, p3_})
        connect(c, &QComboBox::currentIndexChanged, this, pinsMoved);
    refreshExclusion();
    form->addRow("TX:", tx_);
    form->addRow("RX:", rx_);
    form->addRow("MISO:", p3_);
    setPinRow(p3_, QString(), false);
    baud_ = new QLabel("Baud: auto", this);
    baud_->setObjectName("caption");
    form->addRow(baud_);
    root->addLayout(form);

    summary_ = new QLabel(this);
    summary_->setObjectName("hint");
    root->addWidget(summary_);

    text_ = new QPlainTextEdit(this);
    text_->setReadOnly(true);
    text_->setFont(theme::mono(9.5));
    text_->setLineWrapMode(QPlainTextEdit::NoWrap);   // one transaction per line, scroll sideways
    text_->setPlaceholderText("Decoded text (whole capture)");
    root->addWidget(text_, 1);

    frames_ = new FrameListWidget(this);
    root->addWidget(frames_, 2);
    connect(frames_, &FrameListWidget::frameActivated, this, &ProtocolPanel::frameActivated);
    connect(frames_, &FrameListWidget::selectionCleared, this, &ProtocolPanel::frameDeselected);

    setMinimumWidth(220);
}

void ProtocolPanel::fillChannelCombo(QComboBox* c, int select) {
    c->addItem("None", -1);
    for (int i = 0; i < NUM_CH; ++i) c->addItem(QString("D%1").arg(i), i);
    c->setCurrentIndex(select + 1);
}

ProtocolPanel::PinSetup ProtocolPanel::pinSetup() const {
    PinSetup s;
    if (protocol_.isEmpty()) return s;
    auto ch = [](const QComboBox* b) { return b->currentData().toInt(); };
    auto add = [&](const char* role, int c) { if (c >= 0) s.channels.push_back({role, static_cast<uint8_t>(c)}); };
    bool ok = false;
    if (protocol_ == "I2C")      { add("SDA", ch(tx_)); add("SCL", ch(rx_)); ok = ch(tx_) >= 0 && ch(rx_) >= 0; }
    else if (protocol_ == "SPI") { add("CLK", ch(tx_)); add("MOSI", ch(rx_)); add("MISO", ch(p3_)); ok = ch(tx_) >= 0 && (ch(rx_) >= 0 || ch(p3_) >= 0); }
    else if (protocol_ == "CAN") { add("RX", ch(rx_) >= 0 ? ch(rx_) : ch(tx_)); ok = !s.channels.empty(); }
    else                         { add("TX", ch(tx_)); add("RX", ch(rx_)); ok = !s.channels.empty(); }
    if (ok) s.protocol = protocol_; else s.channels.clear();
    return s;
}

std::vector<QComboBox*> ProtocolPanel::activePins() const {
    if (protocol_ == "SPI") return {tx_, rx_, p3_};
    return {tx_, rx_};
}

/// A pin can only have one role: grey out, in every other picker, the channels already chosen.
void ProtocolPanel::refreshExclusion() {
    const std::vector<QComboBox*> all = {tx_, rx_, p3_};
    for (QComboBox* me : all) {
        auto* model = qobject_cast<QStandardItemModel*>(me->model());
        for (int i = 1; i < me->count(); ++i) {              // item 0 is "None"
            bool taken = false;
            for (QComboBox* other : all)
                if (other != me && other->isVisibleTo(this) && other->currentData().toInt() == me->itemData(i).toInt())
                    taken = true;
            auto* item = model->item(i);
            item->setFlags(taken ? item->flags() & ~(Qt::ItemIsEnabled | Qt::ItemIsSelectable)
                                 : item->flags() | Qt::ItemIsEnabled | Qt::ItemIsSelectable);
        }
    }
}

void ProtocolPanel::setPinRow(QComboBox* c, const QString& label, bool visible) {
    if (auto* l = qobject_cast<QLabel*>(form_->labelForField(c))) {
        if (!label.isEmpty()) l->setText(label);
        l->setVisible(visible);
    }
    c->setVisible(visible);
}

void ProtocolPanel::emitPins() {
    if (protocol_.isEmpty()) return;
    QVector<int> chans;
    for (const QComboBox* c : activePins()) {
        const int ch = c->currentData().toInt();
        if (ch >= 0) chans << ch;
    }
    if (!chans.isEmpty()) emit pinsChanged(chans);
}

void ProtocolPanel::emitRoles() {
    QStringList roles;
    for (int i = 0; i < NUM_CH; ++i) roles << QString();
    if (!protocol_.isEmpty()) {
        const bool i2c = (protocol_ == "I2C"), spi = (protocol_ == "SPI");
        const QStringList names = spi ? QStringList{"CLK", "MOSI", "MISO"}
                                : i2c ? QStringList{"SDA", "SCL"}
                                      : QStringList{"TX", "RX"};
        const auto pins = activePins();
        for (std::size_t k = 0; k < pins.size() && k < static_cast<std::size_t>(names.size()); ++k) {
            const int ch = pins[k]->currentData().toInt();
            if (ch >= 0 && ch < NUM_CH) roles[ch] = names[static_cast<int>(k)];
        }
    }
    emit pinRolesChanged(roles);
}

void ProtocolPanel::dropAnnotations() {
    if (annot_active_) { annot_active_ = false; emit annotationsCleared(); }
}

void ProtocolPanel::setProtocol(const QString& name) {
    last_sig_.clear();
    cache_valid_ = false;
    dropAnnotations();
    protocol_ = name;
    title_->setText(name.isEmpty() ? QString() : name);
    // The two pin pickers double as TX/RX (UART) and SDA/SCL (I2C).
    const bool i2c = (name == "I2C");
    const bool can = (name == "CAN");
    const bool spi = (name == "SPI");
    setPinRow(tx_, spi ? "CLK:"  : i2c ? "SDA:" : can ? "TX:" : "TX:", true);
    setPinRow(rx_, spi ? "MOSI:" : i2c ? "SCL:" : can ? "RX:" : "RX:", true);
    setPinRow(p3_, "MISO:", spi);
    // Pins are left for the user to choose; start from None on every protocol switch.
    for (QComboBox* c : {tx_, rx_, p3_}) c->setCurrentIndex(0);
    refreshExclusion();
    baud_->setText(spi ? "SCK: --" : can ? "Bit rate: auto" : i2c ? "Speed: --" : "Baud: auto");
    text_->setPlaceholderText((i2c || can || spi) ? "Hex log (whole capture)" : "Decoded text (whole capture)");
    clear_before_ns_ = -1.0;
    frames_->clear();
    text_->clear();
    summary_->clear();
    emitRoles();
}

void ProtocolPanel::clearDecoded() {
    // The decoder re-decodes the whole history each refresh (to stay locked to
    // real start bits), so "clear" = ignore frames up to the newest edge now.
    if (session_) clear_before_ns_ = session_->digital_buffer().time_range_ns().second;
    text_->clear();
    frames_->clear();
    summary_->clear();
}

void ProtocolPanel::updateFrom(const escope::CaptureSession& session,
                               double view_t0_ns, double view_t1_ns) {
    session_ = &session;
    if (protocol_.isEmpty()) return;

    const auto& buf = session.digital_buffer();

    // Decoding walks the whole history, so only redo it when its inputs changed
    // (new data, pins, Clear). If only the view moved (pan / zoom while paused) the
    // cached events are listed again for the new window, and if nothing at all
    // changed (paused, or between bursts) there is nothing to do.
    bool redecode;
    {
        std::vector<double> sig;
        for (const QComboBox* c : activePins()) sig.push_back(static_cast<double>(c->currentIndex()));
        sig.push_back(clear_before_ns_);
        sig.push_back(live_ ? 1.0 : 0.0);
        sig.push_back(buf.time_range_ns().second);
        for (const QComboBox* c : activePins()) {
            const int ch = c->currentData().toInt();
            if (ch >= 0) sig.push_back(static_cast<double>(buf.edge_count(static_cast<uint8_t>(ch))));
        }
        const bool data_changed = !cache_valid_ || sig != last_sig_;
        const bool view_changed = view_t0_ns != last_view0_ || view_t1_ns != last_view1_;
        if (!data_changed && !view_changed) return;
        last_sig_   = sig;
        last_view0_ = view_t0_ns;
        last_view1_ = view_t1_ns;
        redecode    = data_changed;
    }

    std::vector<escope::DecodedEvent> events;
    const bool i2c = (protocol_ == "I2C");
    const bool can = (protocol_ == "CAN");
    const bool spi = (protocol_ == "SPI");
    std::size_t glitches = 0;      // short pulses the decoder ignored (I2C)

    if (redecode) {
    // While the capture runs, decode only the newest bursts: decoding the whole (huge) history
    // on every refresh takes longer than the bursts arrive and the screen would stall.
    const escope::DigitalBuffer* src = &buf;
    std::unique_ptr<escope::DigitalBuffer> window;
    if (live_) {
        const double t_end = buf.time_range_ns().second;
        auto edges = buf.edges_in_range(t_end - LIVE_WINDOW_NS, t_end + 1.0);
        auto first = std::find_if(edges.begin(), edges.end(),
                                  [](const escope::DigitalEdge& e) { return e.snapshot; });
        if (first != edges.end()) {                    // start at a burst boundary
            window = std::make_unique<escope::DigitalBuffer>(static_cast<uint8_t>(NUM_CH));
            window->push_batch(&*first, static_cast<std::size_t>(edges.end() - first));
            for (double t : buf.burst_ends())
                if (t >= first->timestamp_ns) window->mark_burst_end(t);
            src = window.get();
        }
    }
    if (spi) {
        const int clk = tx_->currentData().toInt(), mosi = rx_->currentData().toInt(),
                  miso = p3_->currentData().toInt();
        if (clk < 0 || (mosi < 0 && miso < 0)) {
            summary_->setText("Select CLK and MOSI or MISO");
            baud_->setText("SCK: --");
            frames_->clear();
            text_->clear();
            dropAnnotations();
            return;
        }
        std::vector<escope::DecoderChannelMap> map = {{"CLK", static_cast<uint8_t>(clk)}};
        if (mosi >= 0) map.push_back({"MOSI", static_cast<uint8_t>(mosi)});
        if (miso >= 0) map.push_back({"MISO", static_cast<uint8_t>(miso)});
        escope::SPIDecoder dec;
        events = dec.decode(*src, map);
        if (dec.clock_hz() > 0) {
            const double hz = dec.clock_hz();
            baud_->setText(QString("SCK ~%1, mode %2%3")
                .arg(hz >= 1e6 ? QString::number(hz / 1e6, 'f', 2) + " MHz" : QString::number(hz / 1e3, 'f', 1) + " kHz")
                .arg(dec.mode_used()).arg(dec.mode_assumed() ? " (assumed)" : ""));
        } else {
            baud_->setText("SCK: no clock");
        }
    } else if (can) {
        // TX = what this node drives, RX = what it sees; each is decoded on its own.
        const struct { const QComboBox* box; const char* role; } lines[] = {{tx_, "TX"}, {rx_, "RX"}};
        QStringList rate_text;
        bool any = false;
        for (const auto& l : lines) {
            const int line = l.box->currentData().toInt();
            if (line < 0) continue;
            any = true;
            escope::CANDecoder dec;
            auto ev = dec.decode(*src, {{"RX", static_cast<uint8_t>(line)}});
            rate_text << (dec.bitrate_used()
                ? QString("%1: %2 kbit/s").arg(l.role).arg(dec.bitrate_used() / 1000.0, 0, 'f', 1)
                : QString("%1: no signal").arg(l.role));
            events.insert(events.end(), ev.begin(), ev.end());
        }
        if (!any) {
            summary_->setText("Select a CAN TX or RX pin");
            baud_->setText("Bit rate: auto");
            frames_->clear();
            text_->clear();
            dropAnnotations();
            return;
        }
        baud_->setText("Bit rate (auto): " + rate_text.join("   "));
    } else if (i2c) {
        const int sda = tx_->currentData().toInt();
        const int scl = rx_->currentData().toInt();
        if (sda < 0 || scl < 0) {
            summary_->setText("Select SDA and SCL pins");
            baud_->setText("Speed: --");
            frames_->clear();
            text_->clear();
            dropAnnotations();
            return;
        }
        escope::I2CDecoder dec;
        // Decode the full history; only the visible window is listed below.
        events = dec.decode(*src, {{"SCL", static_cast<uint8_t>(scl)},
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
            frames_->clear();
            text_->clear();
            dropAnnotations();
            return;
        }

        QStringList baud_text;
        // TX and RX are detected separately so they don't need to match.
        for (const auto& l : lines_to_decode) {
            const uint32_t baud = escope::UartDecoder::detect_baud(*src, static_cast<uint8_t>(l.ch));
            if (!baud) { baud_text << QString("%1: no signal").arg(l.role); continue; }
            baud_text << QString("%1: %2").arg(l.role).arg(baud);

            escope::UartDecoder dec;
            dec.configure({{"baud", std::to_string(baud)}});
            // Decode the full history so framing stays locked to real start bits;
            // only the visible window is listed below.
            auto ev = dec.decode(*src, {{l.role, static_cast<uint8_t>(l.ch)}});
            events.insert(events.end(), ev.begin(), ev.end());
        }
        baud_->setText("Baud (auto): " + baud_text.join("   "));
    }

    events.erase(std::remove_if(events.begin(), events.end(),
        [&](const escope::DecodedEvent& e) { return e.start_ns <= clear_before_ns_; }),
        events.end());

    // Rebuild the whole decoded stream as text, independent of the visible
    // window, so a long string isn't cut off by zoom/scroll. Per line (TX/RX).
    if (spi) {
        std::sort(events.begin(), events.end(),
            [](const auto& a, const auto& b) { return a.start_ns < b.start_ns; });
        // One line per chip-select window (or per burst of clocks) and data line:
        //   <time>  MOSI  [2A] 00 EF        [..] = command (DC low)     MISO  FF 00 00
        // Words that were cut off show as !<label>.
        const int mosi_ch = rx_->currentData().toInt();
        std::vector<std::pair<double, QString>> timed;
        std::map<int, std::pair<double, QString>> cur;           // channel -> start time, text
        auto flush2 = [&](int ch) {
            auto it = cur.find(ch);
            if (it != cur.end() && !it->second.second.isEmpty()) timed.push_back(it->second);
            cur.erase(ch);
        };
        std::map<int, double> last_end, last_dur;
        for (const auto& e : events) {
            using T = escope::DecodedEvent::Type;
            const int ch = e.channel;
            const QString lbl = QString::fromStdString(e.label);
            const QString who = ch == mosi_ch ? "MOSI" : "MISO";
            if (e.type == T::Control) {
                flush2(ch);                                       // CS, /CS or ~: the next word starts a new line
                continue;
            }
            // without chip select, a long pause between words starts a new line
            if (cur.count(ch) && last_dur[ch] > 0 && e.start_ns - last_end[ch] > 4.0 * last_dur[ch]) flush2(ch);
            auto& c = cur[ch];
            if (c.second.isEmpty()) c = {e.start_ns, QString("%1  %2 ").arg(formatTime(e.start_ns), 12).arg(who)};
            if (e.type == T::Address)    c.second += QString(" [%1]").arg(QString::number(e.value, 16).toUpper().rightJustified(2, '0'));
            else if (e.type == T::Data)  c.second += " " + QString::number(e.value, 16).toUpper().rightJustified(2, '0');
            else                         c.second += " !" + lbl;
            last_end[ch] = e.end_ns;
            last_dur[ch] = e.end_ns - e.start_ns;
        }
        for (auto it = cur.begin(); it != cur.end(); ) { const int ch = it->first; ++it; flush2(ch); }
        std::stable_sort(timed.begin(), timed.end(), [](const auto& a, const auto& b) { return a.first < b.first; });
        QStringList lines;
        for (const auto& t : timed) lines << t.second;
        constexpr int MAX_LINES_LOG = 5000;
        if (lines.size() > MAX_LINES_LOG) lines = lines.mid(lines.size() - MAX_LINES_LOG);
        text_->setPlainText(lines.join('\n'));
        text_->verticalScrollBar()->setValue(text_->verticalScrollBar()->maximum());
    } else if (can) {
        std::sort(events.begin(), events.end(),
            [](const auto& a, const auto& b) { return a.start_ns < b.start_ns; });
        // One line per frame and per pin:  <time> TX  ID 0x123  DLC 8  01 02 ...  CRC OK  ACK
        // Each pin builds its own line, so TX and RX frames never interleave.
        std::vector<std::pair<double, QString>> done;
        std::map<int, std::pair<double, QString>> open_line;
        auto flush = [&](int ch) {
            auto it = open_line.find(ch);
            if (it != open_line.end() && !it->second.second.isEmpty()) done.push_back(it->second);
            open_line.erase(ch);
        };
        for (const auto& e : events) {
            using T = escope::DecodedEvent::Type;
            const int ch = e.channel;
            const QString lbl = QString::fromStdString(e.label);
            if (lbl == "SOF") {
                flush(ch);
                open_line[ch] = {e.start_ns, QString("%1 %2").arg(formatTime(e.start_ns), 12)
                                     .arg(ch == tx_->currentData().toInt() ? " TX" : " RX")};
            } else if (lbl == "EOF") flush(ch);
            else {
                auto& ln = open_line[ch];
                if (ln.second.isEmpty()) ln = {e.start_ns, QString("%1 %2").arg(formatTime(e.start_ns), 12)
                                     .arg(ch == tx_->currentData().toInt() ? " TX" : " RX")};
                if (e.type == T::Data) ln.second += " " + lbl.mid(2);
                else ln.second += "  " + lbl;
            }
        }
        { std::vector<int> chs; for (auto& kv : open_line) chs.push_back(kv.first); for (int c : chs) flush(c); }
        std::stable_sort(done.begin(), done.end(), [](const auto& x, const auto& y) { return x.first < y.first; });
        QStringList lines;
        for (const auto& d : done) lines << d.second;
        constexpr int MAX_LINES_LOG = 5000;
        if (lines.size() > MAX_LINES_LOG) lines = lines.mid(lines.size() - MAX_LINES_LOG);
        text_->setPlainText(lines.join('\n'));
        text_->verticalScrollBar()->setValue(text_->verticalScrollBar()->maximum());
    } else if (i2c) {
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

    // Hand the whole decoded stream (not just the visible window) to the waveform:
    // it lays it out for whatever zoom is current, one lane per channel.
    {
        std::sort(events.begin(), events.end(),
            [](const auto& a, const auto& b) { return a.start_ns < b.start_ns; });
        for (const QComboBox* c : activePins()) {
            const int ch = c->currentData().toInt();
            if (ch < 0) continue;
            if (i2c && c == rx_) continue;                 // I2C: the lane belongs to SDA
            if (spi && c == tx_) continue;                 // SPI: lanes under MOSI and MISO, not the clock
            auto lane = std::make_shared<std::vector<escope::DecodedEvent>>();
            for (const auto& e : events)
                if (e.channel == static_cast<uint8_t>(ch)) lane->push_back(e);
            emit annotationsChanged(ch, std::move(lane));
        }
        annot_active_ = true;
    }
    cache_events_   = events;
    cache_glitches_ = glitches;
    cache_valid_    = true;
    } else {
        events   = cache_events_;
        glitches = cache_glitches_;
    }

    // The list shows every frame of the capture, not just the visible window, so it can be searched
    if (redecode) frames_->setEvents(std::make_shared<const std::vector<escope::DecodedEvent>>(events), live_);
    summary_->setText(i2c && glitches ? QString("%1 glitches ignored").arg(glitches) : QString());
}
