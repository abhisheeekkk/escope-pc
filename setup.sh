#!/usr/bin/env bash
# ─────────────────────────────────────────────────────────────────────────────
# EmbeddedScope — Ubuntu setup script
# Run this once on your machine to install dependencies and do a first build.
#
# Usage:
#   chmod +x setup.sh
#   ./setup.sh                    # installs to /data/embeddedscope
#   ./setup.sh --prefix /my/path  # custom path
# ─────────────────────────────────────────────────────────────────────────────

set -euo pipefail

# ── Config ───────────────────────────────────────────────────────────────────
PROJECT_DIR="$(cd "$(dirname "$0")" && pwd)"
BUILD_DIR="${PROJECT_DIR}/build"
JOBS=$(nproc)

echo ""
echo "  ╔════════════════════════════════════════╗"
echo "  ║        EmbeddedScope Setup             ║"
echo "  ║  Embedded Debugging Platform           ║"
echo "  ╚════════════════════════════════════════╝"
echo ""
echo "  Project : ${PROJECT_DIR}"
echo "  Build   : ${BUILD_DIR}"
echo "  CPU cores: ${JOBS}"
echo ""

# ── Detect Ubuntu version ────────────────────────────────────────────────────
if ! command -v lsb_release &>/dev/null; then
    echo "  [!] lsb_release not found — assuming Ubuntu 24.04"
    UBUNTU_VER="24.04"
else
    UBUNTU_VER=$(lsb_release -rs)
    echo "  Ubuntu ${UBUNTU_VER} detected"
fi

# ── Install system dependencies ───────────────────────────────────────────────
echo ""
echo "  [1/4]  Installing system dependencies..."
sudo apt-get update -qq

# Core build tools
sudo apt-get install -y \
    build-essential \
    cmake \
    ninja-build \
    git \
    pkg-config

# Qt6 — LGPL, dynamically linked (compliant)
sudo apt-get install -y \
    qt6-base-dev \
    qt6-base-dev-tools \
    libqt6opengl6-dev \
    qt6-tools-dev \
    qt6-tools-dev-tools

# OpenGL
sudo apt-get install -y \
    libgl1-mesa-dev \
    libgl-dev \
    libglu1-mesa-dev \
    libglfw3-dev

# USB (for real hardware in Phase 2)
sudo apt-get install -y \
    libusb-1.0-0-dev \
    udev

# Optional: ccache for faster rebuilds
if ! command -v ccache &>/dev/null; then
    sudo apt-get install -y ccache
    echo "  ccache installed — subsequent builds will be significantly faster"
fi

echo "  [1/4]  ✓ Dependencies installed"

# ── Configure ────────────────────────────────────────────────────────────────
echo ""
echo "  [2/4]  Configuring with CMake (Debug build)..."
cmake \
    -B "${BUILD_DIR}" \
    -G Ninja \
    -DCMAKE_BUILD_TYPE=RelWithDebInfo \
    -DCMAKE_EXPORT_COMPILE_COMMANDS=ON \
    "${PROJECT_DIR}"

echo "  [2/4]  ✓ Configuration complete"

# ── Build ─────────────────────────────────────────────────────────────────────
echo ""
echo "  [3/4]  Building with ${JOBS} parallel jobs..."
cmake --build "${BUILD_DIR}" -j "${JOBS}"
echo "  [3/4]  ✓ Build complete"

# ── Tests ─────────────────────────────────────────────────────────────────────
echo ""
echo "  [4/4]  Running unit tests..."
cd "${BUILD_DIR}"
ctest --output-on-failure -j "${JOBS}"
echo "  [4/4]  ✓ Tests passed"

# ── Summary ───────────────────────────────────────────────────────────────────
echo ""
echo "  ════════════════════════════════════════════"
echo "  Build successful!"
echo ""
echo "  Binaries:"
echo "    GUI app : ${BUILD_DIR}/bin/embeddedscope"
echo "    CLI tool: ${BUILD_DIR}/bin/escope_cli"
echo "    Tests   : ${BUILD_DIR}/bin/escope_tests"
echo ""
echo "  Quick start:"
echo "    ${BUILD_DIR}/bin/embeddedscope          # launch the GUI"
echo "    ${BUILD_DIR}/bin/escope_cli --help       # headless CLI"
echo "    ${BUILD_DIR}/bin/escope_cli --duration-ms 200 --output /tmp/mysession"
echo ""
echo "  Rebuild after changes:"
echo "    cmake --build ${BUILD_DIR} -j\$(nproc)"
echo ""
echo "  Run tests:"
echo "    cd ${BUILD_DIR} && ctest --output-on-failure"
echo "  ════════════════════════════════════════════"
echo ""
