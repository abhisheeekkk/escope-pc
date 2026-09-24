#include "timeline/TimelineWidget.h"
#include <QPainter>
TimelineWidget::TimelineWidget(QWidget* parent) : QWidget(parent) {
    setMinimumHeight(80);
    setStyleSheet("background: #111;");
}
void TimelineWidget::setSession(const escope::CaptureSession* s) { session_ = s; }
void TimelineWidget::paintEvent(QPaintEvent*) {
    QPainter p(this);
    p.setPen(Qt::darkGray);
    p.drawText(rect(), Qt::AlignCenter, "Protocol Timeline  (Phase 3)");
}
