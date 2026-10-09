#include "panels/ChannelPanel.h"
#include "session/CaptureSession.h"
#include <QPainter>
#include <QMouseEvent>
#include "theme/Theme.h"
#include <vector>
#include <cmath>

ChannelPanel::ChannelPanel(QWidget* parent) : QWidget(parent) {
    setMinimumWidth(236);
    setMouseTracking(true);
    setCursor(Qt::PointingHandCursor);
    setToolTip("Click a channel to show or hide it");
    for (int i = 0; i < 8; ++i) rows_[i].name = QString("D%1").arg(i);
}

static constexpr int kHeadH = 26, kRowH = 36;

int ChannelPanel::rowAt(const QPoint& p) const {
    const int i = (p.y() - kHeadH) / kRowH;
    return (p.y() >= kHeadH && i >= 0 && i < 8) ? i : -1;
}

void ChannelPanel::setChannelVisible(int ch, bool visible) {
    if (ch >= 0 && ch < 8) { visible_[ch] = visible; update(); }
}

void ChannelPanel::mousePressEvent(QMouseEvent* e) {
    const int i = rowAt(e->pos());
    if (i >= 0) emit channelClicked(i);
}

void ChannelPanel::mouseMoveEvent(QMouseEvent* e) {
    const int i = rowAt(e->pos());
    if (i != hover_) { hover_ = i; update(); }
}

void ChannelPanel::leaveEvent(QEvent*) { hover_ = -1; update(); }

void ChannelPanel::setRoles(const QStringList& roles) {
    roles_ = roles;
    update();
}

void ChannelPanel::paintEvent(QPaintEvent*) {
    QPainter p(this);
    p.setRenderHint(QPainter::Antialiasing, true);

    const int W = width(), pad = 14, head_h = kHeadH, row_h = kRowH;

    // Column captions
    p.setFont(theme::ui(8.0, QFont::DemiBold));
    p.setPen(theme::kTextFaint);
    p.drawText(QRect(pad + 14, 2, 90, head_h), Qt::AlignLeft | Qt::AlignVCenter, "CHANNEL");
    p.drawText(QRect(W - pad - 140, 2, 84, head_h), Qt::AlignRight | Qt::AlignVCenter, "FREQUENCY");
    p.drawText(QRect(W - pad - 44, 2, 44, head_h), Qt::AlignCenter, "LEVEL");

    for (int i = 0; i < 8; ++i) {
        const int y = head_h + i * row_h;
        const QRect row(pad - 6, y + 2, W - 2 * pad + 12, row_h - 4);
        const QColor col = theme::channel(i);
        p.setOpacity(visible_[i] ? 1.0 : 0.38);               // hidden channels recede

        if (i == hover_ || i % 2 == 0) {                      // faint banding, brighter under the pointer
            p.setPen(Qt::NoPen);
            p.setBrush(QColor(255, 255, 255, i == hover_ ? 20 : 7));
            p.drawRoundedRect(row, 8, 8);
        }

        p.setPen(Qt::NoPen);
        p.setBrush(col);
        p.drawRoundedRect(QRectF(pad, y + row_h / 2.0 - 8, 3, 16), 1.5, 1.5);

        const QString role = i < roles_.size() ? roles_[i] : QString();
        p.setFont(theme::ui(10.0, QFont::DemiBold));
        p.setPen(theme::kText);
        const QRect name_r(pad + 14, y, 44, row_h);
        p.drawText(name_r, Qt::AlignLeft | Qt::AlignVCenter, rows_[i].name);
        if (!role.isEmpty()) {
            p.setFont(theme::ui(8.5, QFont::Medium));
            p.setPen(col);
            p.drawText(QRect(pad + 50, y, 70, row_h), Qt::AlignLeft | Qt::AlignVCenter, role);
        }

        // A hidden channel is not recorded, so its numbers would be stale
        const QString freq_text = visible_[i] ? rows_[i].freq : QString("Not recorded");
        const bool has_freq = visible_[i] && rows_[i].freq != "N/A" && rows_[i].freq != "No signal";
        p.setFont(theme::mono(9.0));
        p.setPen(has_freq ? theme::kText : theme::kTextFaint);
        p.drawText(QRect(W - pad - 150, y, 94, row_h), Qt::AlignRight | Qt::AlignVCenter, freq_text);

        // Level pill
        const QRectF pill(W - pad - 38, y + row_h / 2.0 - 10, 32, 20);
        const bool hi = visible_[i] && rows_[i].known && rows_[i].level;
        p.setPen(Qt::NoPen);
        p.setBrush(hi ? QColor(col.red(), col.green(), col.blue(), 56) : QColor(255, 255, 255, 14));
        p.drawRoundedRect(pill, 10, 10);
        p.setFont(theme::mono(9.0, QFont::DemiBold));
        p.setPen(hi ? col : theme::kTextMuted);
        p.drawText(pill, Qt::AlignCenter, !visible_[i] ? "Off" : rows_[i].known ? (rows_[i].level ? "1" : "0") : "N/A");
        p.setOpacity(1.0);
    }
}

void ChannelPanel::updateFrom(const escope::CaptureSession& session) {
    int n = std::min(8, (int)escope::CaptureSession::MAX_DIGITAL_CH);
    // Same instant for every channel's level readout -- hoisted out of the
    // loop instead of recomputed (each call was an O(history) scan) once
    // per channel.
    auto [t0, t1] = session.digital_buffer().time_range_ns();
    (void)t0;
    for (int i = 0; i < n; ++i) {
        const auto& info = session.digital_info(i);
        rows_[i].name = QString::fromStdString(info.label);

        // Estimate frequency from the most recent edges only -- this panel
        // never looks further back than that, so there's no reason to copy
        // (and previously, also uselessly re-sort via all_edges() below) the
        // channel's entire history every ~6 Hz tick.
        auto edges = session.digital_buffer().last_edges(i, 40);
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
                rows_[i].freq = fs;
            } else {
                rows_[i].freq = "N/A";
            }
        } else {
            rows_[i].freq = edges.empty() ? "No signal" : "N/A";
        }

        // Current level
        bool lvl = session.digital_buffer().level_at(i, t1);
        rows_[i].level = lvl;
        rows_[i].known = true;
    }
    update();
}
