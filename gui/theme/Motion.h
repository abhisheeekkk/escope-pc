#pragma once
// Small shared animations, so every part of the interface moves the same way.
#include <QEasingCurve>
#include <QGraphicsOpacityEffect>
#include <QPointer>
#include <QPropertyAnimation>
#include <QScrollBar>
#include <QWidget>

namespace motion {

/// Fade a widget in from transparent. Safe to call again while it runs (it just restarts).
/// Not for widgets that contain an OpenGL view.
inline void fadeIn(QWidget* w, int ms = 180) {
    if (!w) return;
    auto* fx = qobject_cast<QGraphicsOpacityEffect*>(w->graphicsEffect());
    if (!fx) {
        fx = new QGraphicsOpacityEffect(w);
        w->setGraphicsEffect(fx);
    }
    fx->setOpacity(0.0);
    auto* a = new QPropertyAnimation(fx, "opacity", fx);
    a->setDuration(ms);
    a->setStartValue(0.0);
    a->setEndValue(1.0);
    a->setEasingCurve(QEasingCurve::OutCubic);
    QPointer<QWidget> guard(w);
    QObject::connect(a, &QPropertyAnimation::finished, fx, [guard] {
        if (guard) guard->setGraphicsEffect(nullptr);     // no effect left behind to slow painting
    });
    a->start(QAbstractAnimation::DeleteWhenStopped);
}

/// Glide a scroll bar to @p value instead of jumping.
inline void scrollTo(QScrollBar* bar, int value, int ms = 200) {
    if (!bar) return;
    if (auto* old = bar->findChild<QPropertyAnimation*>("glide")) old->stop();
    auto* a = new QPropertyAnimation(bar, "value", bar);
    a->setObjectName("glide");
    a->setDuration(ms);
    a->setStartValue(bar->value());
    a->setEndValue(value);
    a->setEasingCurve(QEasingCurve::OutCubic);
    a->start(QAbstractAnimation::DeleteWhenStopped);
}

} // namespace motion
