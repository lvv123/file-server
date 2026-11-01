#pragma once

#include <cstdint>
#include <cstdio>
#include <string>
#include <system_error>

#include "compat.h"
#include "error.h"

namespace fsrv {

// Free bytes available on the volume that holds `dir`.
uint64_t free_bytes(const fs::path& dir);

// Total capacity of the volume that holds `dir`.
uint64_t total_bytes(const fs::path& dir);

// True when both paths live on the same volume / mount point.
bool same_volume(const fs::path& a, const fs::path& b);

// Atomically replaces `dst` with `src`. On POSIX this is rename(2); on Windows
// MoveFileExW(MOVEFILE_REPLACE_EXISTING). Throws ApiError on failure.
void atomic_replace(const fs::path& src, const fs::path& dst);

// Verifies at startup that `tmp_dir` and `files_dir` share a mount point by
// actually renaming a probe file between them. Throws ApiError with a clear
// message when they do not.
void probe_same_volume_or_die(const fs::path& files_dir, const fs::path& tmp_dir);

// Flush + fsync a stdio stream. Throws ApiError on failure.
void sync_stream(std::FILE* f, const std::string& what);

// stat() wrapper: fills is_dir/size/mtime (Unix seconds). Returns false when
// the path does not exist.
bool stat_path(const fs::path& p, bool& is_dir, int64_t& size,
               int64_t& mtime_sec);

// Builds an ApiError from the current errno / GetLastError value.
ApiError io_error(const std::string& what, int err);
ApiError io_error_win32(const std::string& what, unsigned long winerr);

// Maps a std::filesystem/std::error_code failure the same way (the value is a
// Win32 code on Windows and an errno on POSIX).
ApiError io_error_from(const std::string& what, const std::error_code& ec);

}  // namespace fsrv
