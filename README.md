# EmbeddedScope

**Embedded Debugging Platform** — an open-source PC-based instrument for embedded engineers.

> "Show me why my embedded system is behaving incorrectly."

---

## What it is

EmbeddedScope is a PC application that combines:

- **Analog oscilloscope** — 2 channels, GPU-accelerated waveform rendering
- **Logic analyzer** — 8 digital channels, edge-list storage (efficient for long captures)
- **Protocol decoder** — UART and I2C (working), SPI/CAN (Phase 3)
- **Unified timeline** — all signals on one shared nanosecond time base
- **Simulated source** — full GUI development without any hardware
- **eScope hardware source** — 8ch @ 48 MS/s triggered captures over USB CDC-ACM (`/dev/ttyACM*`), STM32-based

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
- [x] eScope (STM32) USB firmware — 8ch @ 48 MS/s raw sample bursts over CDC-ACM (`StmDataSource`)
- [x] Hardware trigger (trigger sample index reported per burst)
- [ ] USB 3 bulk transfer
- [ ] KissFFT integration (replace Phase 1 DFT)
- [ ] Session load (deserializer)

### Phase 3 — Protocol engine
- [ ] SPI decoder
- [x] I2C decoder (START/STOP/repeated START, address + R/W, ACK/NACK, glitch filter)
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

## eScope hardware source

`StmDataSource` reads triggered burst frames from the eScope (STM32) firmware
over USB CDC-ACM (115200 baud). Each frame is a 24-byte header (magic `0xE7`,
version, flags, sample rate, sample count, trigger sample index, sequence
number) followed by that many raw sample bytes, one bit per digital channel.
The GUI toggles between this and the simulated source from the toolbar's
"Connect eScope" button; `enumerate()` looks for `/dev/ttyACM*`.

## Waveform view controls

- **T/div** — pick a preset from the dropdown, or choose "Custom..." to enter
  an exact value (e.g. type `300`, pick `ns`) in a small dialog
- **Cursors** — use the toolbar's "Add Cursor" / "Clear Cursors" buttons
  (clicking the waveform no longer drops a cursor); drag a cursor's badge to
  reassign it as the measurement reference/target, right-click a cursor to
  delete it
- **Pan/zoom** — right-drag or middle-drag to pan, scroll to pan (T/div is
  never changed by scrolling), `L` to resume following live data
- **Channels** — only D0 is shown by default; use the toolbar's "Select"
  menu to show/hide channels (fewer visible channels means less per-frame
  decode/render work)
- **AUTO** — autosets T/div by measuring the visible channel's period from
  its most recent edges and fitting a few cycles across the screen
- **SINGLE** — runs continuously for a ~1s settle delay, then arms
  single-shot mode so the next trigger after that grabs one capture and
  stops; **Run** always resumes continuous capture regardless of a prior
  SINGLE press

---

## Protocol decoder panel

- **Enable** — toolbar "Protocol" menu: UART, I2C or Off. The Protocol dock
  appears/disappears with that menu; it has no float, close or minimize
  buttons and sits in the right-hand column. Resize it by dragging its edge.
- **Pins** — no pins are assigned by default. Pick them yourself: TX/RX for
  UART, SDA/SCL for I2C (a pin can't be used for both roles).
- **UART** — baud rate is auto-detected per line; decoded text shows the whole
  capture.
- **I2C** — shows the bus speed (median SCL period) and a hex log of every
  transaction in the capture, one line per transaction:
  `<time>  S [3C W] A 00 A 21 A P` (`A` = ACK, `N` = NACK, `Sr` = repeated
  START, `~` = burst began mid-transfer, `!INCOMPLETE` / `!NO SYNC` = errors).
  The log keeps the newest 5000 lines.
- **Clear / Copy** — Clear hides everything decoded so far; Copy puts the whole
  log on the clipboard. The log is never cleared by docking/layout changes,
  only by switching protocol or pressing Clear.

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
