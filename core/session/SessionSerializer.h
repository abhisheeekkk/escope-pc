#pragma once

#include "session/CaptureSession.h"
#include <filesystem>
#include <functional>
#include <iomanip>
#include <string>

namespace escope {

/// Serialises and deserialises a CaptureSession to/from disk.
///
/// Directory layout:
///   session/
///     metadata.json       — human-readable session info (written last, so it marks a complete save)
///     analog_ch0.bin      — packed [timestamp_ns(double), voltage(float)] pairs
///     analog_ch1.bin
///     digital_edges.bin   — digital edges (see below)
///
/// digital_edges.bin, format v2 ("ESD2"): per channel, blocks of up to 32768 edges. Each block is
/// delta and varint coded (see EdgeCodec.h) and carries its own CRC-32, so a damaged block is
/// detected and skipped instead of silently producing wrong data. The burst boundaries and the
/// last trigger are stored too, which keeps protocol decoding of a loaded session identical to the
/// live one. Files are written to a temporary name and renamed into place, so a crash or a full
/// disk never leaves a half-written file where a good one used to be.
/// Format v1 (packed 10-byte records) is still read.
///
/// Binary files are little-endian. Format version is in metadata.json.
class SessionSerializer {
public:
    static constexpr int FORMAT_VERSION = 2;

    /// Progress callback: fraction done in 0..1. Return false to cancel. It is always called from the
    /// thread that called save() or load(), never from the worker threads.
    using ProgressFn = std::function<bool(double)>;

    struct SaveReport {
        bool        ok = false;
        std::string message;        ///< what went wrong, or a one-line summary
        std::size_t bytes = 0;      ///< size of digital_edges.bin
        double      seconds = 0.0;
    };
    struct LoadReport {
        bool        ok = false;
        std::string message;            ///< why it failed, or a warning about damage
        std::size_t damaged_blocks = 0; ///< blocks that failed their checksum and were skipped
        std::size_t total_blocks = 0;
        std::size_t edges = 0;
        double      seconds = 0.0;
        bool        legacy_format = false;
    };

    /// Save session to a directory. Creates directory if needed. Never throws.
    static SaveReport save(const CaptureSession& session, const std::filesystem::path& path,
                           ProgressFn progress = nullptr);

    /// Load a session from a directory. Returns nullptr on failure (see @p report). If some blocks
    /// are damaged the rest still loads, and the report says how many were skipped.
    static std::unique_ptr<CaptureSession> load(const std::filesystem::path& path,
                                                LoadReport* report = nullptr,
                                                ProgressFn progress = nullptr);

    /// Export to CSV. Decimates to max_rows (default 10,000) to keep files small.
    /// If t_end_ns > t_start_ns, clips to that time window first.
    /// Columns: timestamp_ns, CH1_V, CH2_V
    static void export_csv(const CaptureSession& session,
                           const std::filesystem::path& path,
                           double t_start_ns = 0.0,
                           double t_end_ns   = 0.0,
                           std::size_t max_rows = 10000);

private:
    static void save_metadata(const CaptureSession& session, const std::filesystem::path& dir);
    static void save_analog  (const CaptureSession& session, const std::filesystem::path& dir);
    static void save_digital (const CaptureSession& session, const std::filesystem::path& dir,
                              const ProgressFn& progress, std::size_t& bytes);
};

} // namespace escope
