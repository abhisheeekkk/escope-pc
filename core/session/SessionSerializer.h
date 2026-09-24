#pragma once

#include "session/CaptureSession.h"
#include <filesystem>
#include <iomanip>
#include <string>

namespace escope {

/// Serialises and deserialises a CaptureSession to/from disk.
///
/// File format (v1):
///   session/
///     metadata.json       — human-readable session info
///     analog_ch0.bin      — packed [timestamp_ns(double), voltage(float)] pairs
///     analog_ch1.bin
///     digital_edges.bin   — packed [timestamp_ns(double), channel(u8), rising(u8)]
///
/// Binary files are little-endian. Format version is in metadata.json.
class SessionSerializer {
public:
    static constexpr int FORMAT_VERSION = 1;

    /// Save session to a directory. Creates directory if needed.
    static void save(const CaptureSession& session, const std::filesystem::path& path);

    /// Load a session from a directory. Returns nullptr on failure.
    static std::unique_ptr<CaptureSession> load(const std::filesystem::path& path);

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
    static void save_digital (const CaptureSession& session, const std::filesystem::path& dir);
};

} // namespace escope
