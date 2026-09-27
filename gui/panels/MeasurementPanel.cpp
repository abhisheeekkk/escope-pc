#include "panels/MeasurementPanel.h"
#include <QGridLayout>
#include <QLabel>
#include <vector>
#include <algorithm>
#include <cmath>
#include <numeric>

MeasurementPanel::MeasurementPanel(QWidget* parent) : QWidget(parent) {
    auto* layout = new QGridLayout(this);
    layout->setSpacing(4);
    layout->setContentsMargins(6, 6, 6, 6);

    auto mkHdr = [&](const QString& t, int row) {
        auto* l = new QLabel(t, this);
        l->setStyleSheet("color:#00E666; font-size:11px; font-weight:bold;");
        layout->addWidget(l, row, 0, 1, 2, Qt::AlignCenter);
    };

    auto mkRow = [&](const QString& name, QLabel*& lbl, int row) {
        auto* nl = new QLabel(name + ":", this);
        nl->setStyleSheet("color:#777; font-size:11px;");
        lbl = new QLabel("---", this);
        lbl->setStyleSheet("color:#00E666; font-family:monospace; font-size:11px;");
        layout->addWidget(nl,  row, 0, Qt::AlignRight);
        layout->addWidget(lbl, row, 1, Qt::AlignLeft);
    };

    mkHdr("-- Selected Channel --", 0);
    int r = 1;
    mkRow("Channel",  lbl_ch_,     r++);
    mkRow("Freq",     lbl_freq_,   r++);
    mkRow("Period",   lbl_period_, r++);
    mkRow("Duty",     lbl_duty_,   r++);
    mkRow("PW high",  lbl_pw_hi_,  r++);
    mkRow("PW low",   lbl_pw_lo_,  r++);
    mkRow("Edges",    lbl_edges_,  r++);
    mkRow("Level",    lbl_level_,  r++);

    layout->setRowStretch(r, 1);
    auto* hint = new QLabel("Click cursor badge\nto select pair", this);
    hint->setStyleSheet("color:#444; font-size:9px;");
    hint->setAlignment(Qt::AlignCenter);
    layout->addWidget(hint, r+1, 0, 1, 2);

    setMaximumWidth(200);
    setMinimumWidth(160);
}

void MeasurementPanel::setFocusChannel(int ch) { focus_ch_ = ch; }

static QString fmt_t(double ns) {
    if (ns <= 0 || !std::isfinite(ns)) return "---";
    if (ns >= 1e9)  return QString::number(ns/1e9,'f',4)  + " s";
    if (ns >= 1e6)  return QString::number(ns/1e6,'f',3)  + " ms";
    if (ns >= 1e3)  return QString::number(ns/1e3,'f',3)  + " us";
    return           QString::number(ns,'f',1)             + " ns";
}
static QString fmt_hz(double f) {
    if (f <= 0 || !std::isfinite(f)) return "---";
    if (f >= 1e6) return QString::number(f/1e6,'f',3) + " MHz";
    if (f >= 1e3) return QString::number(f/1e3,'f',3) + " kHz";
    return         QString::number(f,'f',2)            + " Hz";
}

void MeasurementPanel::updateFrom(const escope::CaptureSession& session) {
    int ch = focus_ch_;
    if (ch < 0 || ch >= (int)escope::CaptureSession::MAX_DIGITAL_CH) return;

    const auto& info = session.digital_info(ch);
    lbl_ch_->setText(QString::fromStdString(info.label));

    // Only the most recent ~100 edges are ever used below -- avoid copying
    // the channel's entire retained history (which can be millions of
    // edges) just to look at the tail of it.
    auto edges = session.digital_buffer().last_edges(ch, 100);

    if (edges.size() < 2) {
        lbl_freq_  ->setText("---");
        lbl_period_->setText("---");
        lbl_duty_  ->setText("---");
        lbl_pw_hi_ ->setText("---");
        lbl_pw_lo_ ->setText("---");
        lbl_edges_ ->setText(QString::number(edges.size()));
        auto [t0,t1] = session.digital_buffer().time_range_ns();
        bool lvl = session.digital_buffer().level_at(ch, t1);
        lbl_level_ ->setText(lvl ? "HIGH" : "LOW");
        return;
    }

    // Use the most recent ~100 edges for accuracy
    int start = std::max(0, (int)edges.size() - 100);
    auto sub = std::vector<escope::DigitalEdge>(edges.begin()+start, edges.end());

    // Collect high-pulse widths and low-pulse widths
    std::vector<double> hi_widths, lo_widths, periods;

    for (int j = 0; j+1 < (int)sub.size(); ++j) {
        double width = sub[j+1].timestamp_ns - sub[j].timestamp_ns;
        if (sub[j].rising)  hi_widths.push_back(width);  // was rising = high pulse
        else                lo_widths.push_back(width);   // was falling = low pulse
    }

    // Period from rising-to-rising
    double prev_rise = -1;
    for (const auto& e : sub) {
        if (e.rising) {
            if (prev_rise > 0) periods.push_back(e.timestamp_ns - prev_rise);
            prev_rise = e.timestamp_ns;
        }
    }

    auto median = [](std::vector<double> v) -> double {
        if (v.empty()) return 0;
        std::sort(v.begin(), v.end());
        return v[v.size()/2];
    };

    double med_hi  = median(hi_widths);
    double med_lo  = median(lo_widths);
    double med_per = median(periods);
    if (med_per <= 0 && med_hi > 0 && med_lo > 0)
        med_per = med_hi + med_lo;

    double freq = med_per > 0 ? 1e9 / med_per : 0;
    double duty = (med_per > 0 && med_hi > 0) ? 100.0 * med_hi / med_per : 0;

    lbl_freq_  ->setText(fmt_hz(freq));
    lbl_period_->setText(fmt_t(med_per));
    lbl_duty_  ->setText(med_per > 0 ? QString::number(duty,'f',1)+" %" : "---");
    lbl_pw_hi_ ->setText(fmt_t(med_hi));
    lbl_pw_lo_ ->setText(fmt_t(med_lo));
    lbl_edges_ ->setText(QString::number(edges.size()));

    auto [t0,t1] = session.digital_buffer().time_range_ns();
    bool lvl = session.digital_buffer().level_at(ch, t1);
    lbl_level_ ->setText(lvl ? "HIGH" : "LOW");
}
