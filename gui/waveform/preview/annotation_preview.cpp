// Developer tool: renders the protocol annotation lane for a simulated I2C bus at
// several zoom levels into PNG files, so the look can be checked without the
// OpenGL window. Usage: annotation_preview <output_dir> [spi]
// (the optional spi argument renders a simulated SPI TFT bus instead of the I2C bus)
#include "waveform/AnnotationPainter.h"
#include "decoders/i2c/I2CDecoder.h"
#include "decoders/spi/SPIDecoder.h"
#include "decoders/base/AnnotationLayout.h"
#include "acquisition/DigitalBuffer.h"

#include <QGuiApplication>
#include <QImage>
#include <QPainter>
#include <cmath>
#include <cstdio>

using namespace escope;

namespace {
constexpr uint8_t SCL = 0, SDA = 1;
constexpr double SAMPLE_NS = 1e9 / 48e6;

struct Bus {
    DigitalBuffer buf{2};
    double last[2] = {0, 0};
    bool   lvl[2]  = {true, true};
    double t = 2000;
    // measured on the scope board at 400 kHz: SCL low 1.67 us, high 0.87 us
    double low = 1670, high = 870, delay = 150;

    Bus() { buf.push_edge(0, SCL, true); buf.push_edge(0, SDA, true); }
    void edge(uint8_t ch, double tn, bool v) {
        if (lvl[ch] == v) return;
        double q = std::round(tn / SAMPLE_NS) * SAMPLE_NS;
        if (q <= last[ch]) q = last[ch] + SAMPLE_NS;
        lvl[ch] = v; last[ch] = q; buf.push_edge(q, ch, v);
    }
    void bit(bool v) {
        edge(SDA, t + delay, v);
        edge(SCL, t + low, true);
        t += low + high;
        edge(SCL, t, false);
    }
    void start(bool repeated = false) {
        if (repeated) { edge(SDA, t + delay, true); edge(SCL, t + low, true); t += low; edge(SDA, t + 300, false); t += 600; }
        else          { edge(SDA, t, false); t += 600; }
        edge(SCL, t, false);
    }
    void byte(uint8_t v, bool ack) { for (int i = 7; i >= 0; --i) bit((v >> i) & 1); bit(!ack); }
    void stop() { edge(SDA, t + delay, false); edge(SCL, t + low, true); edge(SDA, t + low + 300, true); t += low + 600; }
    void idle(double ns) { t += ns; }
};

void trace(QPainter& p, const DigitalBuffer& b, uint8_t ch, double t0, double ppn, int W,
           float hi, float lo, const QColor& c) {
    auto e = b.edges_for_channel(ch);
    p.setPen(QPen(c, 1.5));
    float xp = 0, yp = b.level_at(ch, t0) ? hi : lo;
    for (const auto& ed : e) {
        float x = static_cast<float>((ed.timestamp_ns - t0) * ppn);
        if (x < 0) { yp = ed.rising ? hi : lo; continue; }
        if (x > W) break;
        float yn = ed.rising ? hi : lo;
        p.drawLine(QPointF(xp, yp), QPointF(x, yp));
        p.drawLine(QPointF(x, yp), QPointF(x, yn));
        xp = x; yp = yn;
    }
    p.drawLine(QPointF(xp, yp), QPointF(W, yp));
}
}


// A simulated SPI TFT write (mode 0, 500 kHz): CASET with four parameters, then pixel data.
int render_spi(const QString& dir) {
    constexpr uint8_t CLK = 0, MOSI = 1, CS = 2, DC = 3;
    DigitalBuffer buf{4};
    bool lvl[4] = {false, false, true, true};
    double t = 4000;
    for (uint8_t c = 0; c < 4; ++c) buf.push_edge(0, c, lvl[c]);
    auto set = [&](uint8_t ch, bool v) { if (lvl[ch] != v) { lvl[ch] = v; buf.push_edge(std::round(t / SAMPLE_NS) * SAMPLE_NS, ch, v); } };
    const double bit = 2000;
    auto send = [&](uint8_t v) {
        for (int i = 7; i >= 0; --i) {
            set(MOSI, (v >> i) & 1); t += bit / 2; set(CLK, true); t += bit / 2; set(CLK, false);
        }
        t += 600;                                          // gap between bytes (driver overhead)
    };
    set(CS, false); t += 500;
    set(DC, false); send(0x2A); set(DC, true);
    for (uint8_t v : {0x00, 0x08, 0x00, 0x1A}) send(v);
    set(CS, true); t += 4000; set(CS, false); t += 500;
    set(DC, false); send(0x2C); set(DC, true);
    for (int i = 0; i < 24; ++i) send(static_cast<uint8_t>(i & 1 ? 0xF8 : 0x00));
    set(CS, true); t += 40000;
    const double t_end = t;

    SPIDecoder dec;
    auto ev = std::make_shared<std::vector<DecodedEvent>>(
        dec.decode(buf, {{"CLK", CLK}, {"MOSI", MOSI}, {"CS", CS}, {"DC", DC}}));
    std::printf("decoded %zu events, mode %d, %.0f kHz\n", ev->size(), dec.mode_used(), dec.clock_hz() / 1e3);
    AnnotationIndex idx; idx.build(ev);
    struct V { const char* name; double t0, span; };
    const V views[] = {
        {"1_command_and_params", 3000, 110000},
        {"2_two_transactions",   0,    t_end},
        {"3_pixel_bytes",        90000, 330000},
    };
    const int W = 1500, H = 190;
    for (const auto& v : views) {
        QImage img(W, H, QImage::Format_ARGB32);
        img.fill(QColor(14, 15, 20));
        QPainter p(&img);
        const double ppn = W / v.span;
        p.setPen(QColor(40, 42, 52));
        for (int i = 0; i <= 10; ++i) p.drawLine(QPointF(i * W / 10.0, 0), QPointF(i * W / 10.0, H));
        trace(p, buf, CLK, v.t0, ppn, W, 14, 36, QColor(90, 160, 230));
        trace(p, buf, MOSI, v.t0, ppn, W, 48, 70, QColor(30, 230, 100));
        trace(p, buf, CS, v.t0, ppn, W, 82, 104, QColor(230, 160, 60));
        trace(p, buf, DC, v.t0, ppn, W, 116, 138, QColor(200, 100, 220));
        p.setPen(QColor(150, 150, 150)); p.setFont(QFont("Monospace", 8));
        p.drawText(6, 12, "CLK"); p.drawText(6, 46, "MOSI"); p.drawText(6, 80, "CS"); p.drawText(6, 114, "DC");
        AnnotationView view; view.t0_ns = v.t0; view.t1_ns = v.t0 + v.span; view.px_per_ns = ppn;
        AnnotationLaneGeometry g; g.t0_ns = v.t0; g.px_per_ns = ppn; g.width = W; g.y = 146; g.height = 26;
        paintAnnotationLane(p, idx.items(view), g, QPointF(-1, -1), nullptr);
        p.end();
        const QString path = QString("%1/spi_%2.png").arg(dir, v.name);
        img.save(path);
        std::printf("wrote %s (%zu items)\n", path.toUtf8().constData(), idx.items(view).size());
    }
    return 0;
}

int main(int argc, char** argv) {
    QGuiApplication app(argc, argv);
    const QString dir = argc > 1 ? argv[1] : ".";
    if (argc > 2 && QString(argv[2]) == "spi") return render_spi(dir);

    Bus b;
    b.start();      b.byte(0x78, true); b.byte(0x00, true);
    for (uint8_t v : {0x21, 0x12, 0x35, 0x22, 0x03, 0x03}) b.byte(v, true);
    b.stop();       b.idle(40000);
    b.start();      b.byte(0x78, true); b.byte(0x40, true);
    const uint8_t digits[] = {0x3E,0x51,0x49,0x45,0x3E,0x00, 0x06,0x49,0x49,0x29,0x1E,0x00,
                              0x42,0x61,0x51,0x49,0x46,0x00, 0x42,0x61,0x51,0x49,0x46,0x00,
                              0x3E,0x51,0x49,0x45,0x3E,0x00, 0x3E,0x51,0x49,0x45,0x3E,0x00};
    for (uint8_t v : digits) b.byte(v, true);
    b.stop();       b.idle(120000);
    b.start();      b.byte(0xA0, true); b.byte(0x00, true);             // EEPROM style: write address,
    b.start(true);  b.byte(0xA1, true); b.byte(0xFF, false);            // repeated start, read, NACK
    b.stop();       b.idle(2e6);
    const double t_end = b.t;

    I2CDecoder dec;
    auto ev = std::make_shared<std::vector<DecodedEvent>>(dec.decode(b.buf, {{"SCL", SCL}, {"SDA", SDA}}));
    std::printf("decoded %zu events\n", ev->size());
    AnnotationIndex idx; idx.build(ev);

    struct V { const char* name; double t0, span; };
    const V views[] = {
        {"1_one_byte",       4000,    22000},       // a few bits across the screen
        {"2_few_bytes",      4000,    110000},
        {"3_cursor_write",   0,       260000},      // a whole short transfer
        {"4_data_write",     260000,  900000},      // 38 bytes
        {"5_zoomed_out",     0,       3.2e6},       // transfers become summary bars
        {"6_everything",     0,       t_end},
    };

    const int W = 1500, H = 150;
    for (const auto& v : views) {
        QImage img(W, H, QImage::Format_ARGB32);
        img.fill(QColor(14, 15, 20));
        QPainter p(&img);
        const double ppn = W / v.span;
        // grid
        p.setPen(QColor(40, 42, 52));
        for (int i = 0; i <= 10; ++i) p.drawLine(QPointF(i * W / 10.0, 0), QPointF(i * W / 10.0, H));
        trace(p, b.buf, SCL, v.t0, ppn, W, 18, 44, QColor(90, 160, 230));
        trace(p, b.buf, SDA, v.t0, ppn, W, 62, 88, QColor(30, 230, 100));
        p.setPen(QColor(150, 150, 150)); p.setFont(QFont("Monospace", 8));
        p.drawText(6, 14, "SCL");  p.drawText(6, 58, "SDA");
        p.drawText(QRect(0, H - 14, W, 12), Qt::AlignHCenter,
                   QString("%1 us total, %2 us/div").arg(v.span / 1e3, 0, 'f', 1).arg(v.span / 1e3 / 10, 0, 'f', 2));

        AnnotationView view; view.t0_ns = v.t0; view.t1_ns = v.t0 + v.span; view.px_per_ns = ppn;
        AnnotationLaneGeometry g; g.t0_ns = v.t0; g.px_per_ns = ppn; g.width = W; g.y = 96; g.height = 26;
        paintAnnotationLane(p, idx.items(view), g, QPointF(-1, -1), nullptr);
        p.end();
        const QString path = QString("%1/annot_%2.png").arg(dir, v.name);
        img.save(path);
        std::printf("wrote %s (%zu items)\n", path.toUtf8().constData(), idx.items(view).size());
    }
    return 0;
}
