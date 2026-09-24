// SimulatedSource.cpp -- 16-channel digital logic simulator
// Each channel generates a well-defined, accurately-timed signal.
// #define ANALOG_ENABLED 1 to restore analog generation.

#define ANALOG_ENABLED 0

#include "SimulatedSource.h"
#include <cmath>
#include <chrono>
#include <thread>
#include <iostream>

namespace escope {

SimulatedSource::SimulatedSource(Config cfg) : cfg_(std::move(cfg)) {}
SimulatedSource::~SimulatedSource() { stop(); close(); }

std::vector<DeviceInfo> SimulatedSource::enumerate() {
    return {{"sim:0", "Simulated 16-ch Logic Analyzer", "1.0.0", true}};
}
SourceStatus SimulatedSource::open(const std::string&) {
    open_ = true;
    std::cout << "[SimulatedSource] 16-channel digital simulator ready\n";
    return SourceStatus::OK;
}
void SimulatedSource::close() { stop(); open_ = false; }
SourceStatus SimulatedSource::configure(const CaptureSession&) { return SourceStatus::OK; }

SourceStatus SimulatedSource::start(CaptureSession& session) {
    if (!open_)   return SourceStatus::NotConnected;
    if (running_) return SourceStatus::OK;
    running_ = true;
    worker_ = std::thread(&SimulatedSource::acquisition_loop, this, &session);
    return SourceStatus::OK;
}
void SimulatedSource::stop() {
    running_ = false;
    if (worker_.joinable()) worker_.join();
}

float SimulatedSource::gaussian_noise() {
    rng_state_ = rng_state_ * 1664525u + 1013904223u;
    float u1 = (rng_state_ & 0xFFFFFF) / float(0x1000000);
    rng_state_ = rng_state_ * 1664525u + 1013904223u;
    float u2 = (rng_state_ & 0xFFFFFF) / float(0x1000000);
    return std::sqrt(-2.f * std::log(u1 + 1e-9f)) * std::cos(6.2831853f * u2);
}

// ---- Signal definitions ----------------------------------------------------
// Channel  Signal              Freq      Duty   Notes
// D0       UART TX            115200 bd  ---    bytes 0x55..0xFF cycling
// D1       UART TX             9600 bd  ---    bytes 0xA0..0xBF cycling
// D2       SPI CLK              1 MHz   50%    CPOL=0 CPHA=0
// D3       SPI MOSI             1 MHz   ---    MSB-first, 0xA5 xor 0x5A
// D4       SPI CS (active low)          ---    one 8-bit frame per 10us
// D5       I2C SCL            400 kHz   50%
// D6       I2C SDA            400 kHz   ---    address 0x68 write, reg 0x75
// D7       PWM                 10 kHz   50%
// D8       PWM                  1 kHz   25%
// D9       PWM                500 Hz    75%
// D10      Square              10 kHz   50%
// D11      Square               1 kHz   50%
// D12      Square             100 Hz    50%
// D13      Square              10 Hz    50%
// D14      Interrupt pulse            5us wide every 2ms
// D15      Status LED toggle         every 100ms

void SimulatedSource::acquisition_loop(CaptureSession* session) {
    using namespace std::chrono_literals;

    // Sampling rate: 50 MS/s = 20 ns per step
    const double dt_ns = 1e9 / cfg_.digital_rate; // e.g. 20 ns @ 50MS/s

    // --- Simple square-wave helper (symmetric or asymmetric) ---
    struct SquareGen {
        double period_hi; // ns high
        double period_lo; // ns low
        bool   level    = false;
        double next_ns  = 0.0;

        // Initialise with a phase offset so channels don't all edge at t=0
        void init(double hi, double lo, double phase = 0.0) {
            period_hi = hi; period_lo = lo;
            level = false; next_ns = phase;
        }
        bool tick(double t) {
            if (t < next_ns) return false;
            level    = !level;
            next_ns += level ? period_hi : period_lo;
            return true;
        }
    };

    // --- Per-channel generators ---
    SquareGen sq[16];

    // D7-D13: straight square / PWM
    // Stagger initial phase by 1/8 of period to spread edges across the display
    sq[7].init(50000.0,  50000.0,  0.0);          // D7:  10kHz 50%
    sq[8].init(250000.0, 750000.0, 62500.0);       // D8:   1kHz 25% (250us hi, 750us lo)
    sq[9].init(1500000.0,500000.0, 250000.0);      // D9:  500Hz 75%
    sq[10].init(50000.0, 50000.0,  12500.0);       // D10: 10kHz 50%
    sq[11].init(500000.0,500000.0, 125000.0);      // D11:  1kHz 50%
    sq[12].init(5000000.0,5000000.0,1250000.0);    // D12: 100Hz 50%  (5ms hi)
    sq[13].init(50000000.0,50000000.0,12500000.0); // D13:  10Hz 50%  (50ms hi)
    sq[15].init(100000000.0,100000000.0,25000000.0);// D15: status LED 100ms

    // D14: narrow interrupt pulse -- 5us wide, 2ms period
    double irq_period = 2000000.0; // 2ms
    double irq_width  = 5000.0;    // 5us
    double irq_next   = irq_period; // first pulse at 2ms
    double irq_fall   = 0.0;
    bool   irq_level  = false;

    // --- UART state machine ---
    struct UartGen {
        double bit_ns;
        double next_ns  = 0.0;
        int    state    = 0;   // 0=idle, 1=start, 2..9=bits(LSB first), 10=stop, 11=gap
        uint8_t byte_val= 0x55;
        bool   level    = true; // idle = high

        void init(double baud, double first_ns = 0.0) {
            bit_ns = 1e9 / baud; next_ns = first_ns;
        }
        // Returns true if level changed
        bool tick(double t) {
            if (t < next_ns) return false;
            next_ns += bit_ns;
            bool new_level = true;
            if (state == 0)            { new_level = false; state = 1; }
            else if (state >= 1 && state <= 8) { new_level = (byte_val >> (state-1)) & 1; state++; }
            else if (state == 9)       { new_level = true; state = 10; } // stop bit
            else { // gap between frames
                state = 0; byte_val++;
                if (byte_val == 0) byte_val = 0x55;
                next_ns += bit_ns * 2; // 2-bit gap
                new_level = true;
            }
            bool changed = (new_level != level);
            level = new_level;
            return changed;
        }
    };

    UartGen uart0, uart1;
    uart0.init(115200.0, 0.0);          // D0: first frame at t=0
    uart1.init(9600.0,   0.0);          // D1: first frame at t=0
    uart1.byte_val = 0xA0;

    // --- SPI state machine ---
    struct SpiGen {
        double clk_hp;           // half-period of CLK
        double next_clk    = 0.0;
        bool   clk         = false;
        bool   mosi        = false;
        bool   cs          = true;  // idle = CS high
        double frame_period= 10000.0;  // 10us between frames
        double next_frame  = 0.0;  // first frame immediately
        int    bit         = -1;   // -1 = waiting for frame start
        uint8_t data       = 0xA5;

        void init(double freq_hz, double fp = 10000.0) {
            clk_hp = 0.5e9 / freq_hz; frame_period = fp;
        }
    };
    SpiGen spi; spi.init(1e6, 10000.0);

    // --- I2C 400kHz state variables ---
    double i2c_next_tx  = 0.0;
    bool   i2c_in_tx    = false;
    bool   i2c_scl      = true;
    bool   i2c_sda      = true; // used in I2C state machine
    int    i2c_bit      = 0;
    double i2c_scl_next = 0.0;
    const  double i2c_scl_hp = 0.5e9 / 400000.0;
    static constexpr uint8_t i2c_addr_byte = 0xD0;
    static constexpr uint8_t i2c_data_byte = 0x75;
    int    i2c_state = 0;
    (void)i2c_sda; (void)i2c_bit; // used via assignments in state machine

    // ---- Helper: push edge only if level changed since last stored value -----
    // We track per-channel previous level to avoid redundant edges
    bool prev_level[16] = {};
    prev_level[0]  = true;  // UART idle = high
    prev_level[1]  = true;
    prev_level[4]  = true;  // SPI CS idle = high
    prev_level[5]  = true;  // I2C SCL idle = high
    prev_level[6]  = true;  // I2C SDA idle = high
    prev_level[7]  = sq[7].level;
    // others default false

    // emit_edge defined as a regular function-like block using t_ns from outer scope
    // We use a macro-style approach since lambdas can't capture VLAs portably
    #define emit_edge(ch_idx, new_lvl) do {         if ((new_lvl) != prev_level[(ch_idx)]) {             session->digital_buffer().push_edge(t_ns, (uint8_t)(ch_idx), (new_lvl));             prev_level[(ch_idx)] = (new_lvl);         }     } while(0)

    // Seed initial state edges at t=0
    double t_ns = 0.0;
    emit_edge(0, true);
    emit_edge(1, true);
    emit_edge(4, true);
    emit_edge(5, true);
    emit_edge(6, true);

    constexpr int BATCH = 2000;

    while (running_) {
        for (int i = 0; i < BATCH && running_; ++i) {

            // -- D0: UART 115200 baud --
            if (uart0.tick(t_ns)) emit_edge(0, uart0.level);

            // -- D1: UART 9600 baud --
            if (uart1.tick(t_ns)) emit_edge(1, uart1.level);

            // -- D2/D3/D4: SPI 1MHz --
            if (t_ns >= spi.next_clk) {
                spi.next_clk += spi.clk_hp;
                spi.clk = !spi.clk;
                emit_edge(2, spi.clk);

                if (spi.clk) { // rising edge -- sample/setup MOSI
                    // Start a new frame?
                    if (spi.cs && t_ns >= spi.next_frame) {
                        spi.cs  = false;
                        spi.bit = 7;
                        spi.mosi = (spi.data >> 7) & 1;
                        emit_edge(4, false);
                        emit_edge(3, spi.mosi);
                    } else if (!spi.cs) {
                        if (spi.bit > 0) {
                            spi.bit--;
                            bool mb = (spi.data >> spi.bit) & 1;
                            emit_edge(3, mb); spi.mosi = mb;
                        } else {
                            // End of frame
                            spi.cs = true;
                            emit_edge(4, true);
                            spi.data = spi.data ^ 0x5A;
                            spi.next_frame = t_ns + spi.frame_period;
                        }
                    }
                }
            }

            // -- D5/D6: I2C 400kHz --
            // Simple bit-banged I2C: START + addr(0x68 W) + data(0x75) + STOP
            // State 0=idle, then cycles through start/bits/ack/stop
            if (!i2c_in_tx && t_ns >= i2c_next_tx) {
                // Generate START: SDA goes low while SCL is high
                i2c_sda = false; emit_edge(6, false);
                i2c_state = 0; i2c_bit = 7;
                i2c_in_tx = true;
                i2c_scl_next = t_ns + i2c_scl_hp; // SCL goes low after half period
                i2c_scl = false; emit_edge(5, false); // SCL low = start of data
            }
            if (i2c_in_tx && t_ns >= i2c_scl_next) {
                i2c_scl = !i2c_scl;
                emit_edge(5, i2c_scl);
                i2c_scl_next += i2c_scl_hp;

                if (!i2c_scl) {
                    // SCL just went low -- update SDA for next bit
                    // State machine: 0..7=addr bits(MSB first), 8=ack, 9..16=data, 17=ack, 18=stop
                    if (i2c_state <= 7) {
                        bool sda_bit = (i2c_addr_byte >> (7 - i2c_state)) & 1;
                        i2c_sda = sda_bit; emit_edge(6, sda_bit);
                        i2c_state++;
                    } else if (i2c_state == 8) {
                        // ACK: SDA pulled low by slave
                        i2c_sda = false; emit_edge(6, false);
                        i2c_state++;
                    } else if (i2c_state <= 17) {
                        bool sda_bit = (i2c_data_byte >> (17 - i2c_state)) & 1;
                        i2c_sda = sda_bit; emit_edge(6, sda_bit);
                        i2c_state++;
                    } else if (i2c_state == 18) {
                        // ACK
                        i2c_sda = false; emit_edge(6, false);
                        i2c_state++;
                    } else {
                        // STOP: SDA goes high while SCL is high
                        // First raise SCL
                        i2c_scl = true; emit_edge(5, true);
                        i2c_scl_next += i2c_scl_hp;
                        // Then raise SDA (STOP condition)
                        i2c_sda = true; emit_edge(6, true);
                        i2c_in_tx = false;
                        i2c_next_tx = t_ns + 20000.0; // 20us gap before next transaction
                    }
                }
            }

            // -- D7-D13, D15: square/PWM generators --
            for (int d = 7; d <= 13; ++d) {
                if (sq[d].tick(t_ns)) emit_edge(d, sq[d].level);
            }
            if (sq[15].tick(t_ns)) emit_edge(15, sq[15].level);

            // -- D14: narrow interrupt pulse (5us wide, every 2ms) --
            if (!irq_level && t_ns >= irq_next) {
                irq_level = true; emit_edge(14, true);
                irq_fall  = t_ns + irq_width;
                irq_next  = t_ns + irq_period;
            }
            if (irq_level && t_ns >= irq_fall) {
                irq_level = false; emit_edge(14, false);
            }

            t_ns += dt_ns;
        }

        if (data_cb_) data_cb_();
        std::this_thread::sleep_for(std::chrono::milliseconds(1));
    }
    #undef emit_edge
}

} // namespace escope
