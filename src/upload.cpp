#include "upload.h"

#include <algorithm>
#include <cstdio>
#include <limits>
#include <memory>
#include <mutex>
#include <unordered_map>
#include <unordered_set>

#include "bitmap.h"
#include "db.h"
#include "error.h"
#include "ext.h"
#include "fsops.h"
#include "hpx.h"
#include "repo_files.h"
#include "repo_sessions.h"
#include "util.h"

namespace fsrv {

namespace {

// Big blocks keep the copy loop from being limited by syscall overhead; the
// merge is a pure sequential read/write, so a larger buffer costs nothing but
// memory (a few MiB) and lets the OS pipeline both directions.
constexpr size_t kMergeBuffer = 4 * 1024 * 1024;

std::mutex g_lock_map_mutex;
std::unordered_map<std::string, std::shared_ptr<std::mutex>> g_lock_map;

std::shared_ptr<std::mutex> session_mutex(const std::string& id) {
  std::lock_guard<std::mutex> lk(g_lock_map_mutex);
  auto& slot = g_lock_map[id];
  if (!slot) slot = std::make_shared<std::mutex>();
  return slot;
}

void forget_session_mutex(const std::string& id) {
  std::lock_guard<std::mutex> lk(g_lock_map_mutex);
  g_lock_map.erase(id);
}

fs::path chunk_dir(const AppContext& ctx, const std::string& id) {
  return ctx.tmp_root / fs::u8path(id);
}

std::string chunk_file_name(uint32_t index, const char* suffix) {
  char buf[32];
  std::snprintf(buf, sizeof(buf), "%08u%s", index, suffix);
  return buf;
}

int64_t expected_chunk_size(const SessionRow& row, uint32_t index) {
  if (index + 1 < row.total_chunks) return row.chunk_size;
  const int64_t consumed = static_cast<int64_t>(row.chunk_size) * index;
  return row.total_size - consumed;
}

UploadSessionInfo to_info(const SessionRow& row,
                          const std::vector<uint32_t>& received) {
  UploadSessionInfo info;
  info.id = row.id;
  info.rel_path = row.target_rel_path;
  info.total_size = row.total_size;
  info.total_chunks = row.total_chunks;
  info.chunk_size = row.chunk_size;
  info.created_at = row.created_at;
  info.expires_at = row.expires_at;
  info.received = received;
  return info;
}

// Rebuilds the received-chunk set from what is actually on disk. Chunks that do
// not have the expected size are considered missing (and removed), which makes
// a crash in the middle of a chunk write harmless.
ChunkBitmap bitmap_from_disk(const AppContext& ctx, const SessionRow& row) {
  ChunkBitmap bm(row.total_chunks);
  const fs::path dir = chunk_dir(ctx, row.id);
  std::error_code ec;
  if (!fs::is_directory(dir, ec)) return bm;

  for (fs::directory_iterator it(dir, ec), end; it != end && !ec;
       it.increment(ec)) {
    if (!it->is_regular_file(ec)) continue;
    const std::string name = it->path().filename().u8string();
    if (name.size() != 8 + 5 || name.compare(8, 5, ".part") != 0) continue;
    uint32_t index = 0;
    bool digits = true;
    for (size_t i = 0; i < 8; ++i) {
      if (name[i] < '0' || name[i] > '9') {
        digits = false;
        break;
      }
      index = index * 10 + static_cast<uint32_t>(name[i] - '0');
    }
    if (!digits || index >= row.total_chunks) continue;

    bool is_dir = false;
    int64_t size = 0;
    int64_t mtime = 0;
    if (!stat_path(it->path(), is_dir, size, mtime)) continue;
    if (size != expected_chunk_size(row, index)) {
      fs::remove(it->path(), ec);
      continue;
    }
    bm.set(index);
  }
  return bm;
}

// Creates `rel_dir` below the files root, indexing every level it creates.
void ensure_dirs(const AppContext& ctx, const std::string& rel_dir) {
  if (rel_dir.empty()) return;
  files_make_dirs(ctx.files_root, rel_dir);
}

void remove_tree_quietly(const fs::path& p) {
  std::error_code ec;
  fs::remove_all(p, ec);
  if (ec) log_warn("cannot remove '" + p.u8string() + "': " + ec.message());
}

}  // namespace

UploadSessionInfo upload_init(const AppContext& ctx, std::string_view user_path,
                              int64_t total_size) {
  if (total_size < 0) {
    throw bad_request("total_size must not be negative", "INVALID_TOTAL_SIZE");
  }

  const fs::path target = ctx.paths.resolve(user_path);
  const std::string rel = ctx.paths.to_rel(target);
  if (rel.empty()) {
    throw bad_request("cannot upload onto the server root", "INVALID_TARGET");
  }

  bool is_dir = false;
  int64_t existing_size = 0;
  int64_t existing_mtime = 0;
  if (stat_path(target, is_dir, existing_size, existing_mtime) && is_dir) {
    throw conflict("target path is an existing directory", "TARGET_IS_DIRECTORY");
  }

  const int64_t chunk_size = ctx.cfg.chunk_size_bytes();
  uint64_t total_chunks = 0;
  if (total_size > 0) {
    total_chunks =
        (static_cast<uint64_t>(total_size) + static_cast<uint64_t>(chunk_size) - 1) /
        static_cast<uint64_t>(chunk_size);
  }
  if (total_chunks > std::numeric_limits<uint32_t>::max()) {
    throw bad_request("file is too large for the configured chunk size",
                      "FILE_TOO_LARGE");
  }

  // Reserve room for the incoming bytes plus the temporary copy that exists
  // while the chunks are merged.
  const uint64_t needed = static_cast<uint64_t>(total_size) + 8ULL * 1024 * 1024;
  const uint64_t available = free_bytes(ctx.tmp_root);
  if (available > 0 && available < needed) {
    throw disk_full("not enough free space: need " + human_size(needed) +
                    ", have " + human_size(available));
  }

  const std::string parent_rel = [&] {
    const size_t slash = rel.rfind('/');
    return slash == std::string::npos ? std::string() : rel.substr(0, slash);
  }();
  ensure_dirs(ctx, parent_rel);

  SessionRow row;
  row.id = uuid_v4();
  row.target_rel_path = rel;
  row.total_size = total_size;
  row.total_chunks = static_cast<uint32_t>(total_chunks);
  row.chunk_size = static_cast<uint32_t>(chunk_size);
  row.bitmap = ChunkBitmap(row.total_chunks).blob();
  row.created_at = now_sec();
  row.expires_at = row.created_at + ctx.cfg.session_ttl_seconds();

  const fs::path dir = chunk_dir(ctx, row.id);
  std::error_code ec;
  fs::create_directories(dir, ec);
  if (ec) {
    throw io_error_from("cannot create upload session directory: " + dir.u8string(),
                        ec);
  }

  try {
    session_insert(row);
  } catch (...) {
    remove_tree_quietly(dir);
    throw;
  }

  return to_info(row, {});
}

UploadSessionInfo upload_received(const AppContext& ctx,
                                  std::string_view session_id) {
  const std::string id(session_id);
  auto mutex = session_mutex(id);
  std::lock_guard<std::mutex> lk(*mutex);

  auto row = session_get(id);
  if (!row) {
    throw not_found("upload session not found", "SESSION_NOT_FOUND");
  }
  if (row->expires_at <= now_sec()) {
    remove_tree_quietly(chunk_dir(ctx, id));
    session_delete(id);
    forget_session_mutex(id);
    throw not_found("upload session has expired", "SESSION_EXPIRED");
  }

  ChunkBitmap bm = bitmap_from_disk(ctx, *row);
  if (bm.blob() != row->bitmap) {
    session_update_bitmap(id, bm.blob());
  }
  return to_info(*row, bm.indices());
}

ChunkResult upload_chunk(const AppContext& ctx, std::string_view session_id,
                         uint32_t index, BodyGate& body,
                         bool has_content_length, int64_t content_length) {
  const std::string id(session_id);
  auto mutex = session_mutex(id);
  std::lock_guard<std::mutex> lk(*mutex);

  auto row = session_get(id);
  if (!row) throw not_found("upload session not found", "SESSION_NOT_FOUND");
  if (row->expires_at <= now_sec()) {
    throw not_found("upload session has expired", "SESSION_EXPIRED");
  }
  if (index >= row->total_chunks) {
    throw bad_request("chunk index out of range", "CHUNK_INDEX_OUT_OF_RANGE");
  }

  const int64_t expected = expected_chunk_size(*row, index);

  // The body must always be consumed, otherwise the connection would be left
  // with unread bytes and the next request on this keep-alive connection would
  // be garbage. BodyGate makes the drain idempotent, so the caller's own
  // error-path drain cannot block on an already-exhausted body.
  // A body without a declared length cannot be skipped: httplib reads it until
  // the peer closes the connection, so draining here would hang the worker. The
  // request carries no bytes at all in this case, so there is nothing to skip.
  if (!has_content_length) {
    body.abandon();
    throw length_required("chunk uploads require a Content-Length header");
  }
  if (content_length != expected) {
    body.drain();
    throw too_large("chunk size does not match the declared size (expected " +
                        std::to_string(expected) + " bytes)",
                    "CHUNK_SIZE_MISMATCH");
  }

  const fs::path dir = chunk_dir(ctx, id);
  const fs::path part = dir / fs::u8path(chunk_file_name(index, ".part"));
  const fs::path part_tmp = dir / fs::u8path(chunk_file_name(index, ".part.tmp"));

  std::FILE* f = std::fopen(part_tmp.u8string().c_str(), "wb");
  if (!f) {
    throw io_error("cannot create chunk file", errno);
  }

  int64_t received_total = 0;
  int64_t written = 0;
  bool body_too_large = false;
  bool io_failed = false;
  int io_errno = 0;

  const bool consumed = body.read([&](const char* data, size_t len) {
    received_total += static_cast<int64_t>(len);
    if (!body_too_large && !io_failed) {
      if (received_total > expected) {
        body_too_large = true;
      } else if (std::fwrite(data, 1, len, f) != len) {
        io_failed = true;
        io_errno = errno;
      } else {
        written += static_cast<int64_t>(len);
      }
    }
    return true;
  });

  if (io_failed) {
    std::fclose(f);
    std::error_code ec;
    fs::remove(part_tmp, ec);
    throw io_error("cannot write chunk data", io_errno);
  }
  if (!consumed) {
    std::fclose(f);
    std::error_code ec;
    fs::remove(part_tmp, ec);
    throw bad_request("request body could not be read", "BODY_READ_FAILED");
  }
  if (body_too_large || written != expected) {
    std::fclose(f);
    std::error_code ec;
    fs::remove(part_tmp, ec);
    throw too_large("chunk size does not match the declared size (expected " +
                        std::to_string(expected) + " bytes, received " +
                        std::to_string(written) + ")",
                    "CHUNK_SIZE_MISMATCH");
  }

  try {
    sync_stream(f, "chunk " + std::to_string(index));
  } catch (...) {
    std::fclose(f);
    std::error_code ec;
    fs::remove(part_tmp, ec);
    throw;
  }
  std::fclose(f);

  std::error_code ec;
  fs::rename(part_tmp, part, ec);
  if (ec) {
    const std::error_code rename_ec = ec;
    std::error_code cleanup_ec;
    fs::remove(part_tmp, cleanup_ec);
    throw io_error_from("cannot commit chunk file", rename_ec);
  }

  ChunkBitmap bm = ChunkBitmap::from_blob(row->bitmap, row->total_chunks);
  bm.set(index);
  session_update_bitmap(id, bm.blob());

  ChunkResult result;
  result.received = bm.count();
  result.total_chunks = row->total_chunks;
  result.complete = bm.full();
  return result;
}

void upload_complete(const AppContext& ctx, std::string_view session_id) {
  const std::string id(session_id);
  auto mutex = session_mutex(id);
  std::lock_guard<std::mutex> lk(*mutex);

  auto row = session_get(id);
  if (!row) throw not_found("upload session not found", "SESSION_NOT_FOUND");
  if (row->expires_at <= now_sec()) {
    throw not_found("upload session has expired", "SESSION_EXPIRED");
  }

  ChunkBitmap bm = bitmap_from_disk(ctx, *row);
  if (!bm.full()) {
    session_update_bitmap(id, bm.blob());
    const auto missing = bm.missing();
    const std::string detail =
        std::to_string(missing.size()) + " chunk(s) are missing" +
        (missing.empty() ? "" : ", first missing index: " +
                                   std::to_string(missing.front()));
    throw conflict("upload is incomplete: " + detail, "UPLOAD_INCOMPLETE");
  }

  const fs::path dir = chunk_dir(ctx, id);
  const fs::path merged = dir / fs::u8path("merged.bin");
  const fs::path target = ctx.files_root / fs::u8path(row->target_rel_path);

  const uint64_t available = free_bytes(ctx.tmp_root);
  if (available > 0 && available < static_cast<uint64_t>(row->total_size)) {
    throw disk_full("not enough free space to merge the upload");
  }

  std::FILE* out = std::fopen(merged.u8string().c_str(), "wb");
  if (!out) throw io_error("cannot create merged file", errno);

  std::vector<char> buffer(kMergeBuffer);
  int64_t total_written = 0;
  try {
    for (uint32_t i = 0; i < row->total_chunks; ++i) {
      const fs::path part = dir / fs::u8path(chunk_file_name(i, ".part"));
      std::FILE* in = std::fopen(part.u8string().c_str(), "rb");
      if (!in) throw io_error("cannot open chunk " + std::to_string(i), errno);
      for (;;) {
        const size_t n = std::fread(buffer.data(), 1, buffer.size(), in);
        if (n > 0) {
          if (std::fwrite(buffer.data(), 1, n, out) != n) {
            std::fclose(in);
            throw io_error("cannot write merged file", errno);
          }
          total_written += static_cast<int64_t>(n);
        }
        if (n < buffer.size()) {
          if (std::ferror(in)) {
            std::fclose(in);
            throw io_error("cannot read chunk " + std::to_string(i), errno);
          }
          break;
        }
      }
      std::fclose(in);
    }

    if (total_written != row->total_size) {
      throw bad_request("merged size (" + std::to_string(total_written) +
                            ") does not match the declared size (" +
                            std::to_string(row->total_size) + ")",
                        "SIZE_MISMATCH");
    }

    sync_stream(out, "merged upload");
    std::fclose(out);
    out = nullptr;

    // Make sure the parent directory exists (it may have been deleted while the
    // upload was in flight).
    const size_t slash = row->target_rel_path.rfind('/');
    if (slash != std::string::npos) {
      ensure_dirs(ctx, row->target_rel_path.substr(0, slash));
    }

    atomic_replace(merged, target);

    bool is_dir = false;
    int64_t size = row->total_size;
    int64_t mtime = now_sec();
    stat_path(target, is_dir, size, mtime);
    files_upsert(row->target_rel_path, false, size, mtime);
  } catch (...) {
    if (out) std::fclose(out);
    std::error_code ec;
    fs::remove(merged, ec);
    throw;
  }

  remove_tree_quietly(dir);
  session_delete(id);
  forget_session_mutex(id);
}

int64_t upload_cleanup_expired(const AppContext& ctx) {
  int64_t removed = 0;
  const auto expired = sessions_expired(now_sec());
  for (const auto& row : expired) {
    auto mutex = session_mutex(row.id);
    std::lock_guard<std::mutex> lk(*mutex);
    auto current = session_get(row.id);
    if (current && current->expires_at > now_sec()) continue;
    remove_tree_quietly(chunk_dir(ctx, row.id));
    session_delete(row.id);
    forget_session_mutex(row.id);
    ++removed;
  }
  return removed;
}

int64_t upload_cleanup_orphans(const AppContext& ctx) {
  std::error_code ec;
  if (!fs::is_directory(ctx.tmp_root, ec)) return 0;

  std::unordered_set<std::string> live;
  {
    Stmt st(conn(), "SELECT id FROM upload_sessions");
    while (st.step()) live.insert(st.col_text(0));
  }

  int64_t removed = 0;
  for (fs::directory_iterator it(ctx.tmp_root, ec), end; it != end && !ec;
       it.increment(ec)) {
    if (!it->is_directory(ec)) continue;
    const std::string name = it->path().filename().u8string();
    if (live.find(name) != live.end()) continue;
    remove_tree_quietly(it->path());
    ++removed;
  }
  return removed;
}

}  // namespace fsrv
