#pragma once
// Paints one lane of decoded protocol annotations (S, P, address / data boxes with
// hex values and A / N). Uses only QPainter, so it works on a plain image as well
// as on the OpenGL waveform, and the same code can be previewed offscreen.

#include "decoders/base/AnnotationLayout.h"
#include <QPainter>
#include <QPointF>
#include <QRectF>
#include <QString>
#include <vector>

struct AnnotationHit {
    QRectF  rect;
    QString tip;          ///< text for a tooltip when the mouse is over rect
};

struct AnnotationLaneGeometry {
    double t0_ns      = 0;     ///< time at the left edge
    double px_per_ns  = 0;
    int    width      = 0;
    float  y          = 0;     ///< top of the lane
    float  height     = 26.f;
};

/// Draw @p items into the lane. Boxes that can be hovered are appended to @p hits.
/// @p hover is the mouse position (or a point outside the lane) for the outline.
void paintAnnotationLane(QPainter& p, const std::vector<escope::AnnotationItem>& items,
                         const AnnotationLaneGeometry& g, const QPointF& hover,
                         std::vector<AnnotationHit>* hits);
