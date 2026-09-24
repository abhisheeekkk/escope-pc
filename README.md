# EmbeddedScope

**Embedded Debugging Platform** — an open-source PC-based instrument for embedded engineers.

> "Show me why my embedded system is behaving incorrectly."

---

## What it is

EmbeddedScope is a PC application that combines:

- **Analog oscilloscope** — 2 channels, GPU-accelerated waveform rendering
- **Logic analyzer** — 8 digital channels, edge-list storage (efficient for long captures)
- **Protocol decoder** — UART (working), SPI/I2C/CAN (Phase 3)
- **Unified timeline** — all signals on one shared nanosecond time base
- **Simulated source** — full GUI development without any hardware

---

## License

| Component | License |
|---|---|
| Core C++ library | MIT |
| Qt 6 (dynamically linked) | LGPL 3 |
| KissFFT (Phase 2) | BSD |
| Google Test | BSD |
| OpenGL | Open |

**No GPL dependencies.** The application can be used and distributed freely.

---

## Architecture

```
embeddedscope/
├── core/                   ← Pure C++20, NO Qt. Unit-testable headlessly.
│   ├── acquisition/        ← SampleBuffer, DigitalBuffer, TriggerEngine
│   ├── analysis/           ← Measurements, FFT
│   ├── decoders/           ← Protocol decoder framework + UART/SPI/I2C/CAN
│   ├── session/            ← CaptureSession, SessionSerializer
│   └── hal/                ← IDataSource (hardware abstraction interface)
├── sim/                    ← SimulatedSource (synthetic signals, no hardware needed)
├── gui/                    ← Qt6 application (LGPL, dynamically linked)
│   ├── waveform/           ← OpenGL waveform renderer (min-max decimation)
│   ├── timeline/           ← Unified protocol timeline
│   ├── panels/             ← Measurement and channel panels
│   └── mainwindow/         ← Application shell
├── tools/                  ← escope_cli (headless capture + decode)
└── tests/                  ← Google Test unit tests
```

**Key design rules:**
- `core/` has zero Qt dependency — testable in CI without a display
- All channels share one nanosecond time base (the fundamental requirement for correlation)
- Hardware is abstracted behind `IDataSource` — swap simulated → real USB without touching the GUI

---

## Build

### First time (Ubuntu 22.04 / 24.04)

```bash
cd /data
tar xzf embeddedscope.tar.gz
cd embeddedscope
chmod +x setup.sh
./setup.sh
```

The script installs all dependencies, configures CMake, builds, and runs tests.

### Subsequent builds

```bash
cmake --build build -j$(nproc)
```

### Run

```bash
# GUI application
./build/bin/embeddedscope

# Headless CLI (no display needed)
./build/bin/escope_cli --duration-ms 200
./build/bin/escope_cli --duration-ms 500 --baud 9600 --output /tmp/session1

# Tests
cd build && ctest --output-on-failure
```

---

## Development Roadmap

### Phase 1 — Software only ✅ (current)
- [x] Core data model (SampleBuffer, DigitalBuffer, TriggerEngine)
- [x] CaptureSession — unified data root
- [x] SimulatedSource — 10 MS/s analog + digital edges + UART
- [x] OpenGL waveform renderer with min-max decimation
- [x] Live measurements (Freq, Period, Vpp, Vrms, Duty, Mean)
- [x] UART protocol decoder
- [x] Session serializer (save)
- [x] CLI tool
- [x] Unit test suite
- [x] Pan/zoom polish
- [x] Cursor measurements
- [x] Waveform export (PNG, CSV)
- [x] Session load

### Phase 2 — Real hardware acquisition
- [ ] STM32 / FPGA USB firmware
- [ ] USB 3 bulk transfer
- [ ] Hardware trigger
- [ ] KissFFT integration (replace Phase 1 DFT)
- [ ] Session load (deserializer)

### Phase 3 — Protocol engine
- [ ] SPI decoder
- [ ] I2C decoder
- [ ] CAN decoder
- [ ] Unified protocol timeline rendering
- [ ] Protocol search / filter

### Phase 4 — Correlation
- [ ] Firmware event SDK (debug_event / debug_value)
- [ ] Firmware event overlay on timeline
- [ ] Cross-signal timing measurements
- [ ] "Flight recorder" session format

### Phase 5 — Hardware V1
- [ ] 2 analog + 8 digital PCB
- [ ] ADC selection
- [ ] Analog front end
- [ ] Input protection
- [ ] Manufacturing

---

## Simulated signals (Phase 1)

| Channel | Signal |
|---|---|
| CH1 | 1 kHz sine wave, ±1.65 V |
| CH2 | 500 Hz square wave, 0–3.3 V |
| D0 | UART 115200 baud, sequential bytes |
| D1 | 10 kHz square wave |
| D2 | 1 kHz square wave |

---

## Contributing

- All `core/` changes must come with unit tests
- `core/` must remain Qt-free
- New protocol decoders: implement `IDecoder`, use `REGISTER_DECODER` macro
- New hardware sources: implement `IDataSource`
