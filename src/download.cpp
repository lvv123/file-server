#include "download.h"

#include <algorithm>
#include <cstdio>
#include <memory>
#include <string_view>
#include <vector>

#include "error.h"
#include "fsops.h"
#include "range.h"
#include "util.h"

namespace fsrv {

namespace {

constexpr size_t kReadBuffer = 64 * 1024;

#if defined(_WIN32)
#include <fcntl.h>
#include <io.h>
#include <windows.h>

using FileOffset = __int64;
int seek_file(std::FILE* f, FileOffset offset) {
  return _fseeki64(f, offset, SEEK_SET);
}

// std::fopen opens with _SH_DENYNO, which still omits FILE_SHARE_DELETE: while
// a download is streaming, replacing or deleting the same file fails with a
// sharing violation. Open with every sharing mode allowed instead.
std::FILE* open_for_reading(const fs::path& p) {
  HANDLE h = ::CreateFileW(
      p.wstring().c_str(), GENERIC_READ,
      FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, nullptr,
      OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
  if (h == INVALID_HANDLE_VALUE) {
    throw io_error_win32("cannot open file for reading: " + p.u8string(),
                         ::GetLastError());
  }
  const int fd =
      ::_open_osfhandle(reinterpret_cast<intptr_t>(h), _O_RDONLY | _O_BINARY);
  if (fd == -1) {
    ::CloseHandle(h);
    throw io_error("cannot open file for reading", EMFILE);
  }
  std::FILE* f = ::_fdopen(fd, "rb");
  if (!f) {
    ::_close(fd);
    throw io_error("cannot open file for reading", EBADF);
  }
  return f;
}
#else
using FileOffset = off_t;
int seek_file(std::FILE* f, FileOffset offset) {
  return fseeko(f, offset, SEEK_SET);
}

std::FILE* open_for_reading(const fs::path& p) {
  std::FILE* f = std::fopen(p.u8string().c_str(), "rb");
  if (!f) throw io_error("cannot open file for reading", errno);
  return f;
}
#endif

std::string encode_rfc5987(const std::string& in) {
  static const char* hex = "0123456789ABCDEF";
  std::string out;
  for (unsigned char c : in) {
    const bool unreserved = (c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z') ||
                            (c >= '0' && c <= '9') || c == '-' || c == '.' ||
                            c == '_' || c == '~';
    if (unreserved) {
      out.push_back(static_cast<char>(c));
    } else {
      out.push_back('%');
      out.push_back(hex[(c >> 4) & 0xF]);
      out.push_back(hex[c & 0xF]);
    }
  }
  return out;
}

struct DownloadState {
  std::FILE* file = nullptr;
  int64_t remaining = 0;
  std::vector<char> buffer;

  ~DownloadState() {
    if (file) {
      std::fclose(file);
      file = nullptr;
    }
  }
};

}  // namespace

void download_file(const AppContext& ctx, const httplib::Request& req,
                   httplib::Response& res, RequestScope& scope) {
  // cpp-httplib re-applies Range to every ContentProvider response on its own.
  // We parse Range manually (per the specification), so clear the framework's
  // parsed copy to avoid slicing the already-sliced stream a second time.
  // This relies on `req.ranges` still being the parsed header at handler time;
  // the assert below fails loudly if an upgrade changes that contract.
  static_assert(
      std::string_view(CPPHTTPLIB_VERSION) == std::string_view("0.18.3"),
      "cpp-httplib changed: re-verify manual Range handling (206/416) and "
      "this workaround before bumping the dependency");
  const_cast<httplib::Request&>(req).ranges.clear();

  const fs::path path = ctx.paths.resolve(req.get_param_value("path"));
  const std::string rel = ctx.paths.to_rel(path);
  scope.set_path(rel);

  bool is_dir = false;
  int64_t size = 0;
  int64_t mtime = 0;
  if (!stat_path(path, is_dir, size, mtime)) {
    throw not_found("file not found", "FILE_NOT_FOUND");
  }
  if (is_dir) {
    throw bad_request("path is a directory, not a file", "TARGET_IS_DIRECTORY");
  }

  scope.set_size(size);

  const std::string name = path.filename().u8string();
  res.set_header("Accept-Ranges", "bytes");
  res.set_header("Content-Disposition",
                 "attachment; filename*=UTF-8''" + encode_rfc5987(name));
  res.set_header("Last-Modified", format_time_iso(mtime) + " GMT");

  uint64_t start = 0;
  uint64_t end = size > 0 ? static_cast<uint64_t>(size) - 1 : 0;
  bool partial = false;

  if (req.has_header("Range")) {
    ByteRange range;
    const RangeOutcome outcome =
        parse_range(req.get_header_value("Range"), static_cast<uint64_t>(size),
                    range);
    if (outcome == RangeOutcome::Unsatisfiable) {
      res.status = 416;
      res.set_header("Content-Range", "bytes */" + std::to_string(size));
      res.set_content("", "text/plain");
      return;
    }
    if (outcome == RangeOutcome::Satisfiable) {
      partial = true;
      start = range.start;
      end = range.end;
    }
  }

  const uint64_t length = (size == 0) ? 0 : (end - start + 1);

  std::FILE* f = open_for_reading(path);

  if (size > 0 && seek_file(f, static_cast<FileOffset>(start)) != 0) {
    std::fclose(f);
    throw io_error("cannot seek in file", errno);
  }

  auto state = std::make_shared<DownloadState>();
  state->file = f;
  state->remaining = static_cast<int64_t>(length);
  state->buffer.resize(kReadBuffer);

  if (partial) {
    res.status = 206;
    res.set_header("Content-Range", "bytes " + std::to_string(start) + "-" +
                                        std::to_string(end) + "/" +
                                        std::to_string(size));
  }

  res.set_content_provider(
      static_cast<size_t>(length), "application/octet-stream",
      [state](size_t /*offset*/, size_t length_hint, httplib::DataSink& sink) {
        if (state->remaining <= 0) return false;
        size_t want = static_cast<size_t>(
            std::min<int64_t>(state->remaining,
                              static_cast<int64_t>(length_hint)));
        want = std::min(want, state->buffer.size());
        if (want == 0) return false;
        const size_t got = std::fread(state->buffer.data(), 1, want, state->file);
        if (got == 0) return false;
        state->remaining -= static_cast<int64_t>(got);
        return sink.write(state->buffer.data(), got);
      },
      [state](bool /*success*/) {
        if (state->file) {
          std::fclose(state->file);
          state->file = nullptr;
        }
      });
}

}  // namespace fsrv
