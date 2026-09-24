#!/usr/bin/env bash
# Run this from /data/embeddedscope to apply all build fixes
set -euo pipefail
echo "Applying build fixes..."

# ── Fix 1: MainWindow.cpp — add QApplication include ─────────────────────────
sed -i 's|#include "SimulatedSource.h"|#include "SimulatedSource.h"\n#include <QApplication>|' \
    gui/mainwindow/MainWindow.cpp

# ── Fix 2: MainWindow.cpp — qualified SimulatedSourceConfig type ──────────────
sed -i 's/SimulatedSource::Config sim_cfg;/escope::SimulatedSourceConfig sim_cfg;/' \
    gui/mainwindow/MainWindow.cpp

# ── Fix 3: tools/escope_cli.cpp — qualified types ─────────────────────────────
sed -i 's/SimulatedSource::Config cfg;/escope::SimulatedSourceConfig cfg;/' \
    tools/escope_cli.cpp
sed -i 's/std::make_unique<CaptureSession>()/std::make_unique<escope::CaptureSession>()/' \
    tools/escope_cli.cpp
sed -i 's/std::make_unique<SimulatedSource>/std::make_unique<escope::SimulatedSource>/' \
    tools/escope_cli.cpp

# ── Fix 4: WaveformWidget.cpp — remove invalid nullptr wheelEvent call ────────
sed -i 's/void WaveformWidget::zoomIn()  { wheelEvent(nullptr); }.*$/void WaveformWidget::zoomIn()  { time_per_div_ns_ *= 0.8; time_per_div_ns_ = std::max(time_per_div_ns_, 10.0); update(); }/' \
    gui/waveform/WaveformWidget.cpp
sed -i '/#include <QPainter>/d'     gui/waveform/WaveformWidget.cpp
sed -i '/#include <QFontMetrics>/d' gui/waveform/WaveformWidget.cpp
sed -i '/QPainter p(this);/d'       gui/waveform/WaveformWidget.cpp
sed -i '/drawLabels();/d'           gui/waveform/WaveformWidget.cpp
sed -i '/\/\/ Overlay text via QPainter/d' gui/waveform/WaveformWidget.cpp

echo "All fixes applied. Rebuilding..."
cmake --build build -j$(nproc)
echo ""
echo "Done! Run: ./build/bin/embeddedscope"
