#pragma once
// One place for the application's look: colours, fonts and the channel palette.
#include <QColor>
#include <QFont>
#include <QStringList>

namespace theme {

// Surfaces, darkest to lightest
inline const QColor kCanvas      (20, 20, 22);      // waveform background
inline const QColor kWindow      (28, 28, 30);      // window and docks
inline const QColor kSurface     (44, 44, 46);      // raised controls
inline const QColor kSurfaceHi   (58, 58, 60);      // hovered controls
inline const QColor kSeparator   (56, 56, 58);
inline const QColor kGrid        (38, 38, 41);
inline const QColor kGridMajor   (50, 50, 54);

// Text
inline const QColor kText        (245, 245, 247);
inline const QColor kTextMuted   (152, 152, 159);
inline const QColor kTextFaint   (99, 99, 102);

// Accents
inline const QColor kAccent      (10, 132, 255);
inline const QColor kGreen       (48, 209, 88);
inline const QColor kRed         (255, 69, 58);
inline const QColor kOrange      (255, 159, 10);

/// Distinct colour for each of the eight capture channels.
inline QColor channel(int ch) {
    static const QColor pal[8] = {
        {255, 214, 10},   // D0 yellow
        {100, 210, 255},  // D1 cyan
        {255, 100, 130},  // D2 pink
        {48, 209, 88},    // D3 green
        {255, 159, 10},   // D4 orange
        {191, 90, 242},   // D5 purple
        {172, 142, 104},  // D6 sand
        {229, 229, 234},  // D7 white
    };
    return pal[((ch % 8) + 8) % 8];
}

/// Cursor colours (X1, X2, ...).
inline QColor cursor(int idx) {
    static const QColor pal[8] = {
        {10, 132, 255}, {255, 159, 10}, {191, 90, 242}, {48, 209, 88},
        {255, 55, 95},  {100, 210, 255}, {255, 214, 10}, {229, 229, 234},
    };
    return pal[((idx % 8) + 8) % 8];
}

inline QString css(const QColor& c) {
    return QString("rgba(%1,%2,%3,%4)").arg(c.red()).arg(c.green()).arg(c.blue()).arg(c.alphaF(), 0, 'f', 3);
}

inline QFont ui(qreal pt = 10.0, QFont::Weight w = QFont::Normal) {
    QFont f;
    // San Francisco where it is installed, then Inter (its closest relative), then clean neutral sans faces
    f.setFamilies({"SF Pro Text", "SF Pro Display", "Inter", "Helvetica Neue", "Lato", "Liberation Sans"});
    f.setPointSizeF(pt);
    f.setWeight(w);
    f.setStyleStrategy(QFont::PreferAntialias);
    return f;
}

inline QFont mono(qreal pt = 9.5, QFont::Weight w = QFont::Normal) {
    QFont f;
    f.setFamilies({"SF Mono", "JetBrains Mono", "Menlo", "DejaVu Sans Mono", "Liberation Mono"});
    f.setPointSizeF(pt);
    f.setWeight(w);
    f.setStyleStrategy(QFont::PreferAntialias);
    f.setFixedPitch(true);
    return f;
}

} // namespace theme
