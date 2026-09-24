/// escope_cli — headless capture and decode tool
///
/// Usage:
///   escope_cli [--duration-ms N] [--baud N] [--output DIR]
///
/// Runs the simulated source for N milliseconds, decodes UART on D0,
/// prints measurements and decoded events, optionally saves a session.

#include "session/CaptureSession.h"
#include "session/SessionSerializer.h"
#include "analysis/Measurements.h"
#include "analysis/FFT.h"
#include "decoders/uart/UartDecoder.h"
#include "SimulatedSource.h"

#include <iostream>
#include <iomanip>
#include <chrono>
#include <thread>
#include <string>
#include <filesystem>

using namespace escope;
using namespace std::chrono_literals;

static void print_measurements(const CaptureSession& session) {
    auto m = compute_measurements(session.analog_buffer(0));
    if (!m.valid) { std::cout << "  (no data)\n"; return; }

    auto hz_str = [](double f) -> std::string {
        if (f >= 1e6) return std::to_string(f / 1e6).substr(0,6) + " MHz";
        if (f >= 1e3) return std::to_string(f / 1e3).substr(0,6) + " kHz";
        return std::to_string(f).substr(0,6) + " Hz";
    };

    std::cout << std::fixed << std::setprecision(4);
    std::cout << "  CH1  Freq:   " << hz_str(m.freq_hz)   << "\n";
    std::cout << "  CH1  Period: " << m.period_ns / 1e3    << " µs\n";
    std::cout << "  CH1  Vpp:    " << m.v_pp               << " V\n";
    std::cout << "  CH1  Vrms:   " << m.v_rms              << " V\n";
    std::cout << "  CH1  Duty:   " << m.duty_pct           << " %\n";
}

static void print_uart_events(const CaptureSession& session, uint32_t baud) {
    UartDecoder dec;
    dec.configure({{"baud", std::to_string(baud)}});

    std::vector<DecoderChannelMap> chmap = {{"TX", 0}};
    auto events = dec.decode(session.digital_buffer(), chmap);

    if (events.empty()) {
        std::cout << "  (no UART frames decoded)\n";
        return;
    }

    for (const auto& e : events) {
        double t_us = e.start_ns / 1e3;
        std::cout << std::fixed << std::setprecision(3)
                  << "  [" << std::setw(10) << t_us << " µs]  "
                  << e.label << "\n";
    }
}

int main(int argc, char* argv[]) {
    int    duration_ms   = 100;
    uint32_t baud        = 115200;
    std::string out_dir  = "";

    for (int i = 1; i < argc; ++i) {
        std::string arg = argv[i];
        if (arg == "--duration-ms" && i + 1 < argc) duration_ms = std::stoi(argv[++i]);
        else if (arg == "--baud"   && i + 1 < argc) baud        = std::stoul(argv[++i]);
        else if (arg == "--output" && i + 1 < argc) out_dir     = argv[++i];
        else if (arg == "--help") {
            std::cout << "Usage: escope_cli [--duration-ms N] [--baud N] [--output DIR]\n";
            return 0;
        }
    }

    std::cout << "EmbeddedScope CLI  —  capture " << duration_ms << " ms\n";
    std::cout << "─────────────────────────────────────────────\n";

    auto session = std::make_unique<escope::CaptureSession>();

    escope::SimulatedSourceConfig cfg;
    cfg.gen_uart   = true;
    cfg.uart_baud  = baud;
    auto source = std::make_unique<escope::SimulatedSource>(cfg);

    source->open();
    source->configure(*session);
    source->start(*session);

    std::this_thread::sleep_for(std::chrono::milliseconds(duration_ms));

    source->stop();
    source->close();

    // ── Results ───────────────────────────────────────────────────────────────
    std::cout << "\nSample counts:\n";
    std::cout << "  CH1 analog: " << session->analog_buffer(0).size() << " samples\n";
    std::cout << "  CH2 analog: " << session->analog_buffer(1).size() << " samples\n";
    std::cout << "  Digital:    " << session->digital_buffer().total_edges() << " edges\n";

    double dur_ms = session->duration_ns() / 1e6;
    std::cout << "  Duration:   " << std::fixed << std::setprecision(2) << dur_ms << " ms\n";

    std::cout << "\nMeasurements (CH1):\n";
    print_measurements(*session);

    std::cout << "\nUART decode (D0, " << baud << " baud):\n";
    print_uart_events(*session, baud);

    // ── FFT peak ─────────────────────────────────────────────────────────────
    FFTConfig fft_cfg;
    fft_cfg.size        = 1024;
    fft_cfg.sample_rate = cfg.sample_rate;
    auto bins = compute_fft(session->analog_buffer(0), fft_cfg);
    if (!bins.empty()) {
        auto peak = *std::max_element(bins.begin(), bins.end(),
            [](const FFTBin& a, const FFTBin& b){ return a.magnitude_db < b.magnitude_db; });
        std::cout << "\nFFT peak: " << std::fixed << std::setprecision(1)
                  << peak.frequency_hz << " Hz  ("
                  << peak.magnitude_db << " dB)\n";
    }

    // ── Save session ──────────────────────────────────────────────────────────
    if (!out_dir.empty()) {
        std::cout << "\nSaving session to: " << out_dir << "\n";
        SessionSerializer::save(*session, out_dir);
    }

    std::cout << "\nDone.\n";
    return 0;
}
