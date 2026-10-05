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
- [x] Hardware trigger (trigger sample index reported per burst; edge and channel, Auto or Normal set from the app)
- [x] Device clock: bursts placed by a device timestamp, not by USB arrival time
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
over USB CDC-ACM (115200 baud). Each frame is a header (magic `0xE7`,
version, flags, sample rate, sample count, trigger sample index, sequence
number; 24 bytes for version 1, 32 bytes for version 2) followed by that many
raw sample bytes, one bit per digital channel.

Version 2 frames also carry the device time of the first sample (a free-running
240 MHz timer on the STM32). The device does not sample while a burst uploads, so
bursts are placed on the timeline by this device clock (`BurstClock` in
`core/hal/BurstFrame.h`) instead of by when they arrived over USB; the gaps
between bursts are then exact and the position of an event no longer wobbles by
the USB arrival jitter. Version 1 firmware still works and falls back to arrival
time.

The GUI toggles between this and the simulated source from the toolbar's
"Connect eScope" button; `enumerate()` looks for `/dev/ttyACM*`.

If Connect eScope never connects, check that the port exists (`ls /dev/ttyACM*`,
`lsusb` should list an STMicroelectronics device) and that nothing else has it
open. A board that runs (its LED blinks) but never shows up on USB is probably
running the signal generator firmware, which never starts USB; its LED toggles
every 100 ms instead of every 500 ms. Flash the scope build (see the firmware
repo). The app needs firmware that sends frame version 2 or 1; both are accepted.

### Trigger setup

The trigger is evaluated on the device. `StmDataSource::configure()` sends the
session's trigger setting to it whenever it changes and on every Run: an edge
(rising, falling or either) on any of D0-D7, and Auto or Normal mode.

- **Default:** D0 rising edge, Auto mode (the device captures anyway after 500 ms
  without a trigger, like a scope's Auto).
- **AUTO / SINGLE** push the mode to the device (SINGLE and Normal wait for a real
  trigger, Auto does not).
- **Trigger position** (how much of the 4.78 ms capture comes before the trigger)
  is fixed at the left edge, so almost the whole window is after the trigger and a
  whole event fits. It is not exposed in the GUI.
- The capture inputs have a pull-down so unconnected channels read low;
  `StmDataSource::set_input_nopull()` can release it per channel for open-drain
  buses. The GUI does not expose it.

### Decoded data on the waveform

After choosing a protocol and its pins in the Protocol menu, the decoded data is
drawn on the main view itself, on a lane under the channel (SDA for I2C, TX and RX
for UART), and the pins' channels are shown automatically:

- **I2C:** `S` (green) for START, `Sr` (orange) for repeated START, `P` (red) for
  STOP, a blue box for the address with its R/W (`3C W`), a teal box per data byte
  in hex, and a small `A` (green) or `N` (red, with a red outline) for ACK or NACK
  at the end of each byte. `~` marks a capture that began mid-transfer; a red box
  with `!` is a decode error.
- **UART:** a box per frame with the hex value, and the character when there is
  room (`LF`, `CR` for line ends).
- **It follows the zoom.** Zoomed in, every byte is its own box. As you zoom out a
  box that can no longer hold its text collapses: a whole transfer becomes one bar
  with a summary (`3C W · 36B`), and when even those get closer than a couple of
  pixels they merge into a `xN` bar. Zoom back in and the detail returns. Hover a
  box for a tooltip (what it is, ACK or NACK, and its time).
- It is live: the lane refreshes about five times a second as bursts arrive. The
  decode only reruns when the data, the pins or Clear changed, so panning and
  zooming while paused cost nothing.
- Rows now follow the visible channels, the same layout the channel labels use,
  so each trace sits level with its label and the lane under it.

The layout (what is drawn at which zoom) is `core/decoders/base/AnnotationLayout`
and is unit tested; the painting is `gui/waveform/AnnotationPainter`. The developer
tool `annotation_preview` renders a simulated I2C bus at several zoom levels to
PNGs without the OpenGL window:

    QT_QPA_PLATFORM=offscreen ./build/bin/annotation_preview /tmp/annotations

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
  Each capture burst is decoded on its own: a burst usually begins and ends
  mid-byte, so the decoder tries the first few start-bit candidates and keeps
  the alignment with the fewest framing errors, and drops a byte that is cut off
  by the end of a burst. Nothing is decoded across the gap between bursts.
- **I2C** — shows the bus speed (median SCL period) and a hex log of every
  transaction in the capture, one line per transaction:
  `<time>  S [3C W] A 00 A 21 A P` (`A` = ACK, `N` = NACK, `Sr` = repeated
  START, `~` = burst began mid-transfer, `!INCOMPLETE` / `!NO SYNC` = errors).
  The log keeps the newest 5000 lines and doesn't wrap: one transaction per
  line, scroll sideways for long ones. Below it is a list of the frames in the
  visible window (`46.153113 s  0x45 ACK`).
- **Timing** — on the waveform each I2C byte box runs from the SCL falling edge
  before its first bit to the SCL falling edge after its ACK clock, so boxes
  meet exactly and the ACK clock is inside; bits are still sampled on rising SCL.
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
