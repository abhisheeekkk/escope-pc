#include "waveform/AnnotationPainter.h"
#include <QFont>
#include <QFontMetrics>
#include <algorithm>
#include <cmath>

void paintAnnotationLane(QPainter& p, const std::vector<escope::AnnotationItem>& items,
                         const AnnotationLaneGeometry& g, const QPointF& hover,
                         std::vector<AnnotationHit>* hits) {
    using K = escope::AnnotationItem::Kind;
    const int   W = g.width;
    const float y = g.y, h = g.height;

    QFont f("Monospace", 8, QFont::Bold);   f.setStyleHint(QFont::Monospace);
    QFont fs("Monospace", 7);               fs.setStyleHint(QFont::Monospace);
    const QFontMetrics fm(f), fms(fs);

    auto fmt_t = [](double ns) -> QString {
        if (ns >= 1e9) return QString::number(ns / 1e9, 'f', 6) + " s";
        if (ns >= 1e6) return QString::number(ns / 1e6, 'f', 4) + " ms";
        return QString::number(ns / 1e3, 'f', 2) + " us";
    };
    auto to_px = [&](double t_ns) { return static_cast<float>((t_ns - g.t0_ns) * g.px_per_ns); };

    p.save();
    p.setRenderHint(QPainter::Antialiasing, true);
    p.fillRect(QRectF(0, y, W, h), QColor(255, 255, 255, 9));

    for (const auto& it : items) {
        const float x0 = to_px(it.start_ns);
        const float x1 = to_px(it.end_ns);
        QRectF box;
        QColor fill, edge;
        QString main = QString::fromStdString(it.text);
        const QString tip  = QString::fromStdString(it.detail) + "\n" + fmt_t(it.start_ns);

        if (it.kind == K::Start || it.kind == K::Repeated || it.kind == K::Stop || it.kind == K::Marker) {
            fill = it.kind == K::Start    ? QColor(34, 160, 80)
                 : it.kind == K::Stop     ? QColor(205, 60, 60)
                 : it.kind == K::Repeated ? QColor(214, 140, 30)
                                          : QColor(105, 110, 120);
            const float w = fm.horizontalAdvance(main) + 10.f;
            box = QRectF(x0 - w * 0.5f, y + 3, w, h - 6);
            if (box.right() < 0 || box.left() > W) continue;
            // thin tick from the lane up to the trace, tying the pill to the SDA edge
            p.setPen(QPen(fill, 1, Qt::DotLine));
            p.drawLine(QPointF(x0, y + 3), QPointF(x0, y - 8));
            p.setPen(Qt::NoPen); p.setBrush(fill);
            p.drawRoundedRect(box, 7, 7);
            p.setPen(Qt::white); p.setFont(f);
            p.drawText(box, Qt::AlignCenter, main);
            if (hits) hits->push_back({box, tip});
            continue;
        }

        // A frame / transfer / dense bar: spans start..end, clamped to the view
        float l = std::max(x0, -2.f), r = std::min(x1, static_cast<float>(W) + 2.f);
        if (r - l < 2.f) r = l + 2.f;
        if (r < 0 || l > W) continue;
        box = QRectF(l, y + 2, r - l, h - 4);

        switch (it.kind) {
            case K::Address: fill = QColor(37, 99, 235, 215);  break;
            case K::Field:   fill = it.error ? QColor(214, 140, 30, 230) : QColor(100, 116, 139, 215); break;
            case K::Data:    fill = QColor(13, 148, 136, 215); break;
            case K::Error:   fill = QColor(220, 38, 38, 230);  break;
            case K::Group:   fill = it.error ? QColor(185, 60, 60, 220) : QColor(71, 85, 105, 225); break;
            default:         fill = QColor(100, 116, 139, 190); break;           // Dense
        }
        edge = it.ack == 0 || it.error ? QColor(255, 120, 120) : fill.lighter(140);

        p.setPen(QPen(edge, it.ack == 0 ? 1.8 : 1.0));
        p.setBrush(fill);
        p.drawRoundedRect(box, 3, 3);

        const float bw = static_cast<float>(box.width());
        const QString ack = it.ack == 1 ? "A" : it.ack == 0 ? "N" : "";
        if (it.kind == K::Group) {
            // zoomed out: the summary if it fits, else just the address part
            QString full = main; full.replace(" . ", " \u00B7 ");
            const QString addr = full.section(" \u00B7 ", 0, 0);
            p.setFont(f); p.setPen(Qt::white);
            if (fm.horizontalAdvance(full) + 8 <= bw)       p.drawText(box, Qt::AlignCenter, full);
            else if (fm.horizontalAdvance(addr) + 6 <= bw)  p.drawText(box, Qt::AlignCenter, addr);
        } else if (it.kind == K::Dense) {
            const QString n = QString("x%1").arg(it.count);
            p.setFont(fs); p.setPen(QColor(235, 240, 245));
            if (fms.horizontalAdvance(n) + 6 <= bw) p.drawText(box, Qt::AlignCenter, n);
        } else if (it.kind == K::Error) {
            p.setFont(f); p.setPen(Qt::white);
            if (bw >= 12) p.drawText(box, Qt::AlignCenter, main);
        } else {                                                  // Address / Data
            // "3C W" doesn't fit a narrow box: fall back to just the address, "3C".
            if (it.kind == K::Address && bw < fm.horizontalAdvance(main) + 6)
                main = main.section(' ', 0, 0);
            const int mw = fm.horizontalAdvance(main);
            const int aw = ack.isEmpty() ? 0 : fm.horizontalAdvance(ack);
            const int sw = it.sub.empty() ? 0 : fms.horizontalAdvance(QString::fromStdString(it.sub));
            p.setFont(f);
            if (!ack.isEmpty() && bw >= mw + aw + 16) {
                // hex centred in the left part, the ACK / NACK letter at the right end
                const QRectF hexbox(box.left(), box.top(), bw - aw - 8, box.height());
                p.setPen(Qt::white);
                p.drawText(hexbox, Qt::AlignCenter, main);
                const QRectF ackbox(box.right() - aw - 6, box.top() + 3, aw + 4, box.height() - 6);
                p.setPen(Qt::NoPen);
                p.setBrush(it.ack == 1 ? QColor(20, 90, 50, 230) : QColor(130, 30, 30, 240));
                p.drawRoundedRect(ackbox, 3, 3);
                p.setPen(it.ack == 1 ? QColor(134, 239, 172) : QColor(252, 165, 165));
                p.drawText(ackbox, Qt::AlignCenter, ack);
            } else if (bw >= mw + 6) {
                p.setPen(Qt::white);
                if (sw && bw >= mw + sw + 18) {                   // UART: hex and the character
                    p.drawText(QRectF(box.left(), box.top(), bw * 0.6, box.height()), Qt::AlignCenter, main);
                    p.setFont(fs); p.setPen(QColor(220, 235, 240));
                    p.drawText(QRectF(box.left() + bw * 0.55, box.top(), bw * 0.45, box.height()),
                               Qt::AlignCenter, QString::fromStdString(it.sub));
                } else {
                    p.drawText(box, Qt::AlignCenter, main);
                }
            }
        }

        if (box.contains(hover)) {
            p.setPen(QPen(Qt::white, 1.5)); p.setBrush(Qt::NoBrush);
            p.drawRoundedRect(box, 3, 3);
        }
        if (hits) hits->push_back({box, tip});
    }
    p.restore();
}
