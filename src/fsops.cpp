#include "fsops.h"

#include <cerrno>
#include <chrono>
#include <cstring>
#include <system_error>
#include <thread>

#include "error.h"
#include "util.h"

#if defined(_WIN32)
#include <windows.h>
#include <io.h>
#else
#include <sys/stat.h>
#include <sys/statvfs.h>
#include <unistd.h>
#endif

namespace fsrv {

namespace {

#if defined(_WIN32)
constexpr unsigned long kErrDiskFull = 112;      // ERROR_DISK_FULL
constexpr unsigned long kErrHandleDiskFull = 39;  // ERROR_HANDLE_DISK_FULL
constexpr unsigned long kErrFileNotFound = 2;
constexpr unsigned long kErrPathNotFound = 3;
constexpr unsigned long kErrAccessDenied = 5;
constexpr unsigned long kErrSharingViolation = 32;
constexpr unsigned long kErrFileExists = 80;
constexpr unsigned long kErrAlreadyExists = 183;
#endif

}  // namespace

ApiError io_error(const std::string& what, int err) {
  const std::string detail = what + ": " + std::strerror(err);
  switch (err) {
    case ENOENT:
      return ApiError(404, "NOT_FOUND", detail);
    case EEXIST:
      return ApiError(409, "TARGET_EXISTS", detail);
#if defined(ENOSPC)
    case ENOSPC:
      return ApiError(507, "INSUFFICIENT_STORAGE", detail);
#endif
#if defined(EDQUOT)
    case EDQUOT:
      return ApiError(507, "INSUFFICIENT_STORAGE", detail);
#endif
    default:
      return ApiError(500, "IO_ERROR", detail);
  }
}

ApiError io_error_win32(const std::string& what, unsigned long winerr) {
  const std::string detail = what + " (win32 error " +
                             std::to_string(winerr) + ")";
  switch (winerr) {
    case kErrDiskFull:
    case kErrHandleDiskFull:
      return ApiError(507, "INSUFFICIENT_STORAGE", detail);
    case kErrFileNotFound:
    case kErrPathNotFound:
      return ApiError(404, "NOT_FOUND", detail);
    case kErrFileExists:
    case kErrAlreadyExists:
      return ApiError(409, "TARGET_EXISTS", detail);
    case kErrAccessDenied:
    case kErrSharingViolation:
      return ApiError(500, "PERMISSION_DENIED", detail);
    default:
      return ApiError(500, "IO_ERROR", detail);
  }
}

ApiError io_error_from(const std::string& what, const std::error_code& ec) {
#if defined(_WIN32)
  return io_error_win32(what, static_cast<unsigned long>(ec.value()));
#else
  return io_error(what, ec.value());
#endif
}

uint64_t free_bytes(const fs::path& dir) {
#if defined(_WIN32)
  ULARGE_INTEGER avail;
  avail.QuadPart = 0;
  if (!GetDiskFreeSpaceExW(dir.wstring().c_str(), &avail, nullptr, nullptr)) {
    return 0;
  }
  return avail.QuadPart;
#else
  struct statvfs st{};
  if (statvfs(dir.c_str(), &st) != 0) return 0;
  return static_cast<uint64_t>(st.f_bavail) * st.f_frsize;
#endif
}

uint64_t total_bytes(const fs::path& dir) {
#if defined(_WIN32)
  ULARGE_INTEGER total;
  total.QuadPart = 0;
  if (!GetDiskFreeSpaceExW(dir.wstring().c_str(), nullptr, &total, nullptr)) {
    return 0;
  }
  return total.QuadPart;
#else
  struct statvfs st{};
  if (statvfs(dir.c_str(), &st) != 0) return 0;
  return static_cast<uint64_t>(st.f_blocks) * st.f_frsize;
#endif
}

bool same_volume(const fs::path& a, const fs::path& b) {
#if defined(_WIN32)
  auto volume_of = [](const fs::path& p) -> uint64_t {
    HANDLE h = CreateFileW(p.wstring().c_str(), 0,
                           FILE_SHARE_READ | FILE_SHARE_WRITE |
                               FILE_SHARE_DELETE,
                           nullptr, OPEN_EXISTING,
                           FILE_FLAG_BACKUP_SEMANTICS, nullptr);
    if (h == INVALID_HANDLE_VALUE) return 0;
    BY_HANDLE_FILE_INFORMATION info{};
    const BOOL ok = GetFileInformationByHandle(h, &info);
    CloseHandle(h);
    if (!ok) return 0;
    return info.dwVolumeSerialNumber;
  };
  const uint64_t va = volume_of(a);
  const uint64_t vb = volume_of(b);
  return va != 0 && va == vb;
#else
  struct stat sa{}, sb{};
  if (stat(a.c_str(), &sa) != 0) return false;
  if (stat(b.c_str(), &sb) != 0) return false;
  return sa.st_dev == sb.st_dev;
#endif
}

void atomic_replace(const fs::path& src, const fs::path& dst) {
#if defined(_WIN32)
  // MoveFileExW refuses to replace a file that another process has open, even
  // when that reader shared delete access: it reports ACCESS_DENIED, which is a
  // permanent refusal rather than a transient lock, so retrying alone never
  // helps. ReplaceFileW is the API that does work against an open target, so
  // fall back to it before giving up.
  constexpr int kAttempts = 5;
  for (int attempt = 1;; ++attempt) {
    if (MoveFileExW(src.wstring().c_str(), dst.wstring().c_str(),
                    MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH)) {
      return;
    }
    DWORD err = GetLastError();
    if (err == ERROR_ACCESS_DENIED || err == ERROR_SHARING_VIOLATION) {
      // ReplaceFileW needs an existing destination; when there is none,
      // MoveFileExW above has already succeeded.
      if (ReplaceFileW(dst.wstring().c_str(), src.wstring().c_str(), nullptr,
                       REPLACEFILE_WRITE_THROUGH, nullptr, nullptr)) {
        return;
      }
      err = GetLastError();
    }
    if ((err == ERROR_SHARING_VIOLATION || err == ERROR_ACCESS_DENIED) &&
        attempt < kAttempts) {
      std::this_thread::sleep_for(std::chrono::milliseconds(50 * attempt));
      continue;
    }
    throw io_error_win32("cannot replace " + dst.u8string(), err);
  }
#else
  if (::rename(src.c_str(), dst.c_str()) != 0) {
    throw io_error("cannot replace " + dst.u8string(), errno);
  }
#endif
}

void sync_stream(std::FILE* f, const std::string& what) {
  if (std::fflush(f) != 0) throw io_error("cannot flush " + what, errno);
#if defined(_WIN32)
  if (_commit(_fileno(f)) != 0) throw io_error("cannot sync " + what, errno);
#else
  if (fsync(fileno(f)) != 0) throw io_error("cannot sync " + what, errno);
#endif
}

bool stat_path(const fs::path& p, bool& is_dir, int64_t& size,
               int64_t& mtime_sec) {
#if defined(_WIN32)
  WIN32_FILE_ATTRIBUTE_DATA data{};
  if (!GetFileAttributesExW(p.wstring().c_str(), GetFileExInfoStandard, &data)) {
    return false;
  }
  is_dir = (data.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) != 0;
  ULARGE_INTEGER sz;
  sz.LowPart = data.nFileSizeLow;
  sz.HighPart = data.nFileSizeHigh;
  size = static_cast<int64_t>(sz.QuadPart);
  ULARGE_INTEGER ft;
  ft.LowPart = data.ftLastWriteTime.dwLowDateTime;
  ft.HighPart = data.ftLastWriteTime.dwHighDateTime;
  // 100ns ticks since 1601-01-01 -> Unix seconds.
  mtime_sec = static_cast<int64_t>(ft.QuadPart / 10000000ULL) - 11644473600LL;
  return true;
#else
  struct stat st{};
  if (::stat(p.c_str(), &st) != 0) return false;
  is_dir = S_ISDIR(st.st_mode);
  size = static_cast<int64_t>(st.st_size);
  mtime_sec = static_cast<int64_t>(st.st_mtime);
  return true;
#endif
}

void probe_same_volume_or_die(const fs::path& files_dir,
                              const fs::path& tmp_dir) {
  if (!same_volume(files_dir, tmp_dir)) {
    throw internal_error(
        "data root is misconfigured: '" + tmp_dir.u8string() + "' and '" +
            files_dir.u8string() +
            "' must be on the same volume/mount point so uploads can be moved "
            "atomically",
        "DATA_ROOT_NOT_ATOMIC");
  }

  const std::string tag =
      ".probe-" + std::to_string(static_cast<long long>(now_ms()));
  const fs::path probe_src = tmp_dir / fs::u8path(tag);
  const fs::path probe_dst = files_dir / fs::u8path(tag + ".out");
  std::error_code ec;

  {
    std::FILE* f = std::fopen(probe_src.u8string().c_str(), "wb");
    if (!f) {
      throw io_error("cannot create probe file in " + tmp_dir.u8string(),
                     errno);
    }
    const char payload = 'x';
    const bool wrote = std::fwrite(&payload, 1, 1, f) == 1;
    const bool flushed = std::fflush(f) == 0;
    std::fclose(f);
    if (!wrote || !flushed) {
      fs::remove(probe_src, ec);
      throw io_error("cannot write probe file in " + tmp_dir.u8string(),
                     errno);
    }
  }

  try {
    atomic_replace(probe_src, probe_dst);
  } catch (const ApiError& e) {
    fs::remove(probe_src, ec);
    throw internal_error(
        "data root is misconfigured: cannot atomically move a file from '" +
            tmp_dir.u8string() + "' to '" + files_dir.u8string() + "': " +
            e.what(),
        "DATA_ROOT_NOT_ATOMIC");
  }

  fs::remove(probe_dst, ec);
}

}  // namespace fsrv
