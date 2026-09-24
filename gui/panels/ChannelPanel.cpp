#include "panels/ChannelPanel.h"
#include "session/CaptureSession.h"
#include <QLabel>
#include <QGridLayout>
#include <QScrollArea>
#include <QVBoxLayout>
#include <vector>
#include <cmath>

ChannelPanel::ChannelPanel(QWidget* parent) : QWidget(parent) {
    auto* grid = new QGridLayout(this);
    grid->setSpacing(2);
    grid->setContentsMargins(4, 4, 4, 4);

    // Header
    auto* h1 = new QLabel("Channel", this);
    auto* h2 = new QLabel("Freq", this);
    auto* h3 = new QLabel("Lvl", this);
    h1->setStyleSheet("color:#888; font-size:10px; font-weight:bold;");
    h2->setStyleSheet("color:#888; font-size:10px; font-weight:bold;");
    h3->setStyleSheet("color:#888; font-size:10px; font-weight:bold;");
    grid->addWidget(h1, 0, 0); grid->addWidget(h2, 0, 1); grid->addWidget(h3, 0, 2);

    // Colour alternates between two greens matching the waveform renderer
    for (int i = 0; i < 16; ++i) {
        QString col = (i % 2 == 0) ? "#00E666" : "#00A676";
        rows_[i].name  = new QLabel(this);
        rows_[i].freq  = new QLabel("---", this);
        rows_[i].level = new QLabel("?", this);
        rows_[i].name ->setStyleSheet(QString("color:%1; font-size:10px; font-family:monospace;").arg(col));
        rows_[i].freq ->setStyleSheet("color:#AAA; font-size:10px; font-family:monospace;");
        rows_[i].level->setStyleSheet(QString("color:%1; font-size:10px; font-family:monospace; font-weight:bold;").arg(col));
        rows_[i].level->setAlignment(Qt::AlignCenter);
        grid->addWidget(rows_[i].name,  i+1, 0);
        grid->addWidget(rows_[i].freq,  i+1, 1);
        grid->addWidget(rows_[i].level, i+1, 2);
    }
    grid->setRowStretch(17, 1);
    setMinimumWidth(160);
}

void ChannelPanel::updateFrom(const escope::CaptureSession& session) {
    int n = std::min(16, (int)escope::CaptureSession::MAX_DIGITAL_CH);
    for (int i = 0; i < n; ++i) {
        const auto& info = session.digital_info(i);
        rows_[i].name->setText(QString::fromStdString(info.label));

        // Estimate frequency from edges
        auto edges = session.digital_buffer().edges_for_channel(i);
        if (edges.size() >= 4) {
            // Collect rising-edge intervals (last 20 at most)
            std::vector<double> periods;
            bool prev_rising = false;
            double prev_t = 0;
            int start = std::max(0, (int)edges.size() - 40);
            for (int j = start; j < (int)edges.size(); ++j) {
                if (edges[j].rising) {
                    if (prev_rising)
                        periods.push_back(edges[j].timestamp_ns - prev_t);
                    prev_t = edges[j].timestamp_ns;
                    prev_rising = true;
                }
            }
            if (periods.size() >= 2) {
                // Median period
                std::sort(periods.begin(), periods.end());
                double period_ns = periods[periods.size() / 2];
                double freq = 1e9 / period_ns;
                QString fs;
                if (freq >= 1e6)      fs = QString::number(freq/1e6,'f',2) + " MHz";
                else if (freq >= 1e3) fs = QString::number(freq/1e3,'f',2) + " kHz";
                else                  fs = QString::number(freq,'f',1) + " Hz";
                rows_[i].freq->setText(fs);
            } else {
                rows_[i].freq->setText("---");
            }
        } else {
            rows_[i].freq->setText(edges.empty() ? "no sig" : "---");
        }

        // Current level
        auto all = session.digital_buffer().all_edges();
        auto [t0, t1] = session.digital_buffer().time_range_ns();
        bool lvl = session.digital_buffer().level_at(i, t1);
        rows_[i].level->setText(lvl ? "1" : "0");
    }
}
