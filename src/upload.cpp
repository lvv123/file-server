#include "upload.h"

#include <algorithm>
#include <cerrno>
#include <cstdio>
#include <limits>
#include <memory>
#include <mutex>
#include <shared_mutex>
#include <unordered_map>
#include <unordered_set>
#include <utility>

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

// Per-session synchronization.
//
// `io` is held *shared* while a chunk body is written, so several chunks of the
// same session can be uploaded at the same time (they touch different part
// files). It is held *exclusive* while the session is merged, deleted or
// reclaimed, which must never overlap with a write still in flight.
//
// `meta` serializes read-modify-write cycles on the session row (the received
// chunk bitmap). It is always acquired after `io`.
struct SessionLocks {
  std::shared_mutex io;
  std::mutex meta;
};

std::mutex g_lock_map_mutex;
std::unordered_map<std::string, std::shared_ptr<SessionLocks>> g_lock_map;

std::shared_ptr<SessionLocks> session_locks(const std::string& id) {
  std::lock_guard<std::mutex> lk(g_lock_map_mutex);
  auto& slot = g_lock_map[id];
  if (!slot) slot = std::make_shared<SessionLocks>();
  return slot;
}

void forget_session_locks(const std::string& id) {
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

// Chunk bodies land in a uniquely named temporary file and are renamed into
// place, so a retry racing a slow original (or two clients uploading the same
// index) cannot corrupt each other. `bitmap_from_disk` only looks at files named
// exactly "<8 digits>.part", so these never show up as received chunks.
std::string chunk_tmp_name(uint32_t index) {
  const std::string id = uuid_v4();
  return chunk_file_name(index, "") + "." + id.substr(0, 8) + ".tmp";
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

// Owns the temporary file of one chunk write. Cleanup is automatic: the
// destructor closes the stream before removing the file (Windows refuses to
// delete a file that is still open) and removes it unless the chunk was
// committed. Keeping this out of the control flow means a newly added error
// path cannot leak a descriptor or leave a *.tmp file behind.
class PartFile {
 public:
  explicit PartFile(fs::path tmp) : tmp_(std::move(tmp)) {}
  ~PartFile() {
    close();
    if (!committed_) {
      std::error_code ec;
      fs::remove(tmp_, ec);
    }
  }

  PartFile(const PartFile&) = delete;
  PartFile& operator=(const PartFile&) = delete;

  bool open() {
    f_ = std::fopen(tmp_.u8string().c_str(), "wb");
    return f_ != nullptr;
  }

  std::FILE* get() const { return f_; }

  void close() {
    if (!f_) return;
    std::FILE* f = f_;
    f_ = nullptr;
    std::fclose(f);
  }

  void commit() { committed_ = true; }

  const fs::path& path() const { return tmp_; }

 private:
  std::FILE* f_ = nullptr;
  fs::path tmp_;
  bool committed_ = false;
};

struct ChunkPlan {
  uint32_t index = 0;
  int64_t expected = 0;
  fs::path part;
  fs::path part_tmp;
};

// Validates everything that can be checked before the body is touched. Body
// handling on the reject paths follows the keep-alive invariant: a declared
// body is drained, a body without a declared length is abandoned (draining it
// would block until the peer closes the connection).
ChunkPlan validate_chunk_request(const AppContext& ctx, const std::string& id,
                                 uint32_t index, BodySource& body,
                                 bool has_content_length,
                                 int64_t content_length) {
  auto row = session_get(id);
  if (!row) {
    forget_session_locks(id);
    throw not_found("upload session not found", "SESSION_NOT_FOUND");
  }
  if (row->expires_at <= now_sec()) {
    throw not_found("upload session has expired", "SESSION_EXPIRED");
  }
  if (index >= row->total_chunks) {
    throw bad_request("chunk index out of range", "CHUNK_INDEX_OUT_OF_RANGE");
  }

  const int64_t expected = expected_chunk_size(*row, index);
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

  ChunkPlan plan;
  plan.index = index;
  plan.expected = expected;
  const fs::path dir = chunk_dir(ctx, id);
  plan.part = dir / fs::u8path(chunk_file_name(index, ".part"));
  plan.part_tmp = dir / fs::u8path(chunk_tmp_name(index));
  return plan;
}

// Copies the body into `part`, verifies the byte count and flushes it to stable
// storage. The temp file stays under PartFile's ownership.
void stream_to_part(BodySource& body, PartFile& part, uint32_t index,
                    int64_t expected) {
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
      } else if (std::fwrite(data, 1, len, part.get()) != len) {
        // Save errno before any close (including the one in ~PartFile) can
        // overwrite it.
        io_failed = true;
        io_errno = errno;
      } else {
        written += static_cast<int64_t>(len);
      }
    }
    return true;
  });

  if (io_failed) throw io_error("cannot write chunk data", io_errno);
  if (!consumed) {
    throw bad_request("request body could not be read", "BODY_READ_FAILED");
  }
  if (body_too_large || written != expected) {
    throw too_large("chunk size does not match the declared size (expected " +
                        std::to_string(expected) + " bytes, received " +
                        std::to_string(written) + ")",
                    "CHUNK_SIZE_MISMATCH");
  }

  sync_stream(part.get(), "chunk " + std::to_string(index));
  part.close();
}

// Publishes the temporary file and records the chunk in the bitmap.
ChunkResult commit_part(const std::string& id, const ChunkPlan& plan,
                        SessionLocks& locks, PartFile& part) {
  std::error_code ec;
  fs::rename(part.path(), plan.part, ec);
  if (ec) throw io_error_from("cannot commit chunk file", ec);
  part.commit();

  // Re-read the row: other chunks of this session may have updated the bitmap
  // while this one was streaming, so the copy read at entry can be stale.
  ChunkResult result{};
  std::lock_guard<std::mutex> meta_lock(locks.meta);
  auto fresh = session_get(id);
  if (!fresh) throw not_found("upload session not found", "SESSION_NOT_FOUND");
  ChunkBitmap bm = ChunkBitmap::from_blob(fresh->bitmap, fresh->total_chunks);
  bm.set(plan.index);
  session_update_bitmap(id, bm.blob());
  result.received = bm.count();
  result.total_chunks = fresh->total_chunks;
  result.complete = bm.full();
  return result;
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

  // Peak usage is two copies: the chunks in tmp/ plus the merged file that is
  // built next to the target. Reject up front instead of failing halfway
  // through the merge.
  const uint64_t size_bytes = static_cast<uint64_t>(total_size);
  const uint64_t needed =
      size_bytes > (UINT64_MAX / 2) ? UINT64_MAX : size_bytes * 2 + 8ULL * 1024 * 1024;
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
  auto locks = session_locks(id);

  {
    std::shared_lock<std::shared_mutex> io_lock(locks->io);
    auto row = session_get(id);
    if (!row) {
      // Drop the lock entry too: otherwise an attacker (or a client with a
      // stale id) grows g_lock_map by one entry per request, forever.
      forget_session_locks(id);
      throw not_found("upload session not found", "SESSION_NOT_FOUND");
    }
    if (row->expires_at > now_sec()) {
      std::lock_guard<std::mutex> meta_lock(locks->meta);
      ChunkBitmap bm = bitmap_from_disk(ctx, *row);
      if (bm.blob() != row->bitmap) {
        session_update_bitmap(id, bm.blob());
      }
      return to_info(*row, bm.indices());
    }
  }

  // Expired: reclaim under the exclusive lock, so that no chunk write or merge
  // can still be running while the directory disappears.
  {
    std::unique_lock<std::shared_mutex> io_lock(locks->io);
    auto row = session_get(id);
    if (row && row->expires_at <= now_sec()) {
      remove_tree_quietly(chunk_dir(ctx, id));
      session_delete(id);
      forget_session_locks(id);
    }
  }
  throw not_found("upload session has expired", "SESSION_EXPIRED");
}

ChunkResult upload_chunk(const AppContext& ctx, std::string_view session_id,
                         uint32_t index, BodySource& body,
                         bool has_content_length, int64_t content_length) {
  const std::string id(session_id);
  auto locks = session_locks(id);
  // Shared: other chunks of this session may be written concurrently, but a
  // merge or a delete cannot start while this one is in flight.
  std::shared_lock<std::shared_mutex> io_lock(locks->io);

  // The body must always be consumed, otherwise the connection would be left
  // with unread bytes and the next request on this keep-alive connection would
  // be garbage. BodySource makes the drain idempotent, so the caller's own
  // error-path drain cannot block on an already-exhausted body.
  const ChunkPlan plan = validate_chunk_request(
      ctx, id, index, body, has_content_length, content_length);

  PartFile part(plan.part_tmp);
  if (!part.open()) {
    throw io_error("cannot create chunk file", errno);
  }

  stream_to_part(body, part, index, plan.expected);
  return commit_part(id, plan, *locks, part);
}

void upload_complete(const AppContext& ctx, std::string_view session_id) {
  const std::string id(session_id);
  auto locks = session_locks(id);
  // Exclusive: a chunk write must not be in flight while the session is merged
  // and then removed.
  std::unique_lock<std::shared_mutex> io_lock(locks->io);

  auto row = session_get(id);
  if (!row) {
    forget_session_locks(id);
    throw not_found("upload session not found", "SESSION_NOT_FOUND");
  }
  if (row->expires_at <= now_sec()) {
    throw not_found("upload session has expired", "SESSION_EXPIRED");
  }

  ChunkBitmap bm = [&] {
    std::lock_guard<std::mutex> meta_lock(locks->meta);
    ChunkBitmap scanned = bitmap_from_disk(ctx, *row);
    if (!scanned.full()) session_update_bitmap(id, scanned.blob());
    return scanned;
  }();
  if (!bm.full()) {
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
    if (!stat_path(target, is_dir, size, mtime)) {
      // The rename succeeded but we cannot read the result back. Do not index
      // guessed metadata: keep the session so the client can retry.
      throw internal_error("cannot stat the merged file after replacing it");
    }
    files_upsert(row->target_rel_path, false, size, mtime);
  } catch (...) {
    if (out) std::fclose(out);
    std::error_code ec;
    fs::remove(merged, ec);
    throw;
  }

  remove_tree_quietly(dir);
  session_delete(id);
  forget_session_locks(id);
}

int64_t upload_cleanup_expired(const AppContext& ctx) {
  int64_t removed = 0;
  const auto expired = sessions_expired(now_sec());
  for (const auto& row : expired) {
    auto locks = session_locks(row.id);
    std::unique_lock<std::shared_mutex> io_lock(locks->io);
    auto current = session_get(row.id);
    if (current && current->expires_at > now_sec()) continue;
    remove_tree_quietly(chunk_dir(ctx, row.id));
    session_delete(row.id);
    forget_session_locks(row.id);
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
