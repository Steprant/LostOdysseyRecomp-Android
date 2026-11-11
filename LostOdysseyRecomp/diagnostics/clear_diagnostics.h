#pragma once
// Steprant 2.0.2 (stable base): "Clear diagnostics" on the Tester page.
//
// The diagnostics tools keep their files in the app's private storage, where
// a file manager cannot reach them: until 2.0.2 the only way to free them was
// "Clear data" for the whole app. A frame capture of the 2.0.1 test builds
// passed 100 MB and went into every later Tester export.
//
// Deleted, under the app's files directory (getFilesDir() on Android):
// - android-diagnostics/ and everything in it: the Tester sessions
//   (session-*), the frame captures (frame-samples/) and anything else a
//   test build wrote there;
// - pgo/*.profraw: the profiles of an instrumented build (SR4 C-4).
// Not touched: config/ (settings, developer switches), the saves, the shader
// and pipeline caches, the GPU drivers, the logs and the signing key; none of
// them lives in those two places.
//
// The caller refuses while a recording runs or its collector is finishing;
// Clear refuses while a frame capture runs.
#include <cstdint>
#include <cwchar>
#include <filesystem>
#include <string>
#include <system_error>
#include <vector>

#include "frame_samples.h"

namespace diagnostics::clear_diagnostics
{
struct Totals {
    uint64_t bytes = 0, files = 0, failed = 0;
};

// The app's files directory: the frame captures go to
// <files>/android-diagnostics/frame-samples (main.cpp). Empty if unknown.
inline std::filesystem::path FilesDirectory()
{
    const auto samples = frame_samples::Directory();
    if (samples.empty() || samples.filename() != "frame-samples" || samples.parent_path().filename() != "android-diagnostics")
        return {};
    return samples.parent_path().parent_path();
}

inline std::filesystem::path DiagnosticsDirectory(const std::filesystem::path& files) { return files / "android-diagnostics"; }
inline std::filesystem::path ProfileDirectory(const std::filesystem::path& files) { return files / "pgo"; }

inline bool IsProfile(const std::filesystem::path& path) { return path.extension() == ".profraw"; }

// Bytes and regular files under path (path itself when it is a file).
inline void Count(const std::filesystem::path& path, Totals& totals)
{
    std::error_code ec;
    if (std::filesystem::is_regular_file(path, ec)) {
        const auto size = std::filesystem::file_size(path, ec);
        if (!ec) totals.bytes += size;
        ++totals.files;
        return;
    }
    if (!std::filesystem::is_directory(path, ec)) return;
    for (std::filesystem::recursive_directory_iterator it(path, std::filesystem::directory_options::skip_permission_denied, ec), end;
         !ec && it != end; it.increment(ec)) {
        std::error_code e;
        if (!it->is_regular_file(e)) continue;
        const auto size = it->file_size(e);
        if (!e) totals.bytes += size;
        ++totals.files;
    }
}

// What Clear would delete.
inline Totals Measure(const std::filesystem::path& files)
{
    Totals totals;
    std::error_code ec;
    for (std::filesystem::directory_iterator it(DiagnosticsDirectory(files), ec), end; !ec && it != end; it.increment(ec))
        Count(it->path(), totals);
    ec.clear();
    for (std::filesystem::directory_iterator it(ProfileDirectory(files), ec), end; !ec && it != end; it.increment(ec)) {
        std::error_code e;
        if (IsProfile(it->path()) && it->is_regular_file(e)) Count(it->path(), totals);
    }
    return totals;
}

// Deletes one entry; what it held goes to `deleted`, or to `failed` if any of
// it remains.
inline void Remove(const std::filesystem::path& path, Totals& deleted)
{
    Totals entry;
    Count(path, entry);
    std::error_code ec;
    std::filesystem::remove_all(path, ec);
    std::error_code e;
    if (ec || std::filesystem::exists(std::filesystem::symlink_status(path, e))) {
        Totals left;
        Count(path, left);
        deleted.bytes += entry.bytes > left.bytes ? entry.bytes - left.bytes : 0;
        deleted.files += entry.files > left.files ? entry.files - left.files : 0;
        deleted.failed += left.files ? left.files : 1;
        return;
    }
    deleted.bytes += entry.bytes;
    deleted.files += entry.files;
}

// False (nothing deleted) while a frame capture runs. The directories
// themselves stay: the Tester and the next capture write into them again.
inline bool Clear(const std::filesystem::path& files, Totals& deleted)
{
    if (!frame_samples::Forget()) return false;
    // The entries are listed first: a directory changed while it is being
    // iterated may skip some.
    std::vector<std::filesystem::path> doomed;
    std::error_code ec;
    for (std::filesystem::directory_iterator it(DiagnosticsDirectory(files), ec), end; !ec && it != end; it.increment(ec))
        doomed.push_back(it->path());
    ec.clear();
    for (std::filesystem::directory_iterator it(ProfileDirectory(files), ec), end; !ec && it != end; it.increment(ec)) {
        std::error_code e;
        if (IsProfile(it->path()) && it->is_regular_file(e)) doomed.push_back(it->path());
    }
    for (const auto& path : doomed) Remove(path, deleted);
    return true;
}

// "136.2" for the Tester page (1 MB = 1048576 bytes).
inline std::wstring Megabytes(uint64_t bytes)
{
    wchar_t text[32];
    std::swprintf(text, sizeof(text) / sizeof(text[0]), L"%.1f", double(bytes) / (1024.0 * 1024.0));
    return text;
}
} // namespace diagnostics::clear_diagnostics
