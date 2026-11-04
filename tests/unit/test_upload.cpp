#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <functional>
#include <string>
#include <utility>

#include "bitmap.h"
#include "db.h"
#include "paths.h"
#include "repo_sessions.h"
#include "test_helpers.h"
#include "upload.h"
#include "util.h"

namespace fs = std::filesystem;

using fsrv::ChunkBitmap;
using fsrv::SessionRow;

namespace {

// In-memory BodySource with hooks that record what the upload code asked the
// transport to do.
class MemoryBody : public fsrv::BodySource {
 public:
  explicit MemoryBody(std::string data, size_t step = 4096)
      : data_(std::move(data)), step_(step) {}

  bool read(const std::function<bool(const char*, size_t)>& sink) override {
    read_called = true;
    size_t offset = 0;
    while (offset < data_.size()) {
      const size_t n = std::min(step_, data_.size() - offset);
      if (!sink(data_.data() + offset, n)) return false;
      offset += n;
      consumed = offset;
      if (fail_read) return false;
    }
    return !fail_read;
  }

  void drain() override { drain_called = true; }
  void abandon() override { abandon_called = true; }

  bool read_called = false;
  bool drain_called = false;
  bool abandon_called = false;
  bool fail_read = false;
  size_t consumed = 0;

 private:
  std::string data_;
  size_t step_;
};

// Fresh database + data root per test case. Sessions are inserted directly with
// a small chunk size so the tests do not have to move megabytes around; every
// field upload_chunk depends on is the same one upload_init would write.
struct UploadFixture {
  testutil::TempDir dir{"fsrv-upload"};
  fsrv::AppContext ctx;

  UploadFixture() {
    const fs::path files = dir / "files";
    const fs::path tmp = dir / "tmp";
    fs::create_directories(files);
    fs::create_directories(tmp);
    fsrv::db_init(dir / "meta.db");
    ctx.data_root = dir.path();
    ctx.files_root = files;
    ctx.tmp_root = tmp;
    ctx.db_path = dir / "meta.db";
    ctx.paths = fsrv::PathGuard::make(files);
  }

  ~UploadFixture() { fsrv::db_close_current_thread(); }

  SessionRow make_session(int64_t total_size, uint32_t chunk_size,
                          std::string rel = "out.bin",
                          int64_t ttl_seconds = 3600) {
    SessionRow row;
    row.id = fsrv::uuid_v4();
    row.target_rel_path = std::move(rel);
    row.total_size = total_size;
    row.chunk_size = chunk_size;
    row.total_chunks =
        total_size == 0
            ? 0
            : static_cast<uint32_t>((total_size + chunk_size - 1) / chunk_size);
    row.bitmap = ChunkBitmap(row.total_chunks).blob();
    row.created_at = fsrv::now_sec();
    row.expires_at = row.created_at + ttl_seconds;
    fsrv::session_insert(row);
    fs::create_directories(ctx.tmp_root / fs::u8path(row.id));
    return row;
  }

  fs::path chunk_dir(const SessionRow& row) const {
    return ctx.tmp_root / fs::u8path(row.id);
  }

  size_t entry_count(const SessionRow& row) const {
    std::error_code ec;
    size_t n = 0;
    for (fs::directory_iterator it(chunk_dir(row), ec), end; !ec && it != end;
         it.increment(ec)) {
      ++n;
    }
    return n;
  }
};

}  // namespace

TEST_CASE(upload_chunk_streams_into_the_part_file) {
  UploadFixture f;
  const SessionRow row = f.make_session(20, 10);
  MemoryBody body("0123456789");

  const fsrv::ChunkResult r =
      fsrv::upload_chunk(f.ctx, row.id, 0, body, true, 10);

  CHECK_EQ(r.received, static_cast<uint32_t>(1));
  CHECK_EQ(r.total_chunks, static_cast<uint32_t>(2));
  CHECK(!r.complete);
  CHECK(body.read_called);
  CHECK_EQ(testutil::read_bytes(f.chunk_dir(row) / "00000000.part"),
           std::string("0123456789"));
  CHECK_EQ(f.entry_count(row), static_cast<size_t>(1));  // no *.tmp left
}

TEST_CASE(upload_chunk_updates_the_bitmap_across_chunks) {
  UploadFixture f;
  const SessionRow row = f.make_session(20, 10);
  MemoryBody first("aaaaaaaaaa");
  MemoryBody second("bbbbbbbbbb");

  const fsrv::ChunkResult r1 =
      fsrv::upload_chunk(f.ctx, row.id, 1, first, true, 10);
  CHECK_EQ(r1.received, static_cast<uint32_t>(1));
  CHECK(!r1.complete);

  const fsrv::ChunkResult r2 =
      fsrv::upload_chunk(f.ctx, row.id, 0, second, true, 10);
  CHECK_EQ(r2.received, static_cast<uint32_t>(2));
  CHECK(r2.complete);

  const auto stored = fsrv::session_get(row.id);
  REQUIRE(stored.has_value());
  const ChunkBitmap bm =
      ChunkBitmap::from_blob(stored->bitmap, stored->total_chunks);
  CHECK(bm.test(0));
  CHECK(bm.test(1));
}

TEST_CASE(upload_chunk_duplicate_index_overwrites_the_part) {
  UploadFixture f;
  const SessionRow row = f.make_session(10, 10);
  MemoryBody first("first-data");
  MemoryBody second("second-dat");

  fsrv::upload_chunk(f.ctx, row.id, 0, first, true, 10);
  const fsrv::ChunkResult r =
      fsrv::upload_chunk(f.ctx, row.id, 0, second, true, 10);

  CHECK_EQ(r.received, static_cast<uint32_t>(1));
  CHECK(r.complete);
  CHECK_EQ(testutil::read_bytes(f.chunk_dir(row) / "00000000.part"),
           std::string("second-dat"));
  CHECK_EQ(f.entry_count(row), static_cast<size_t>(1));
}

TEST_CASE(upload_chunk_without_content_length_is_abandoned) {
  UploadFixture f;
  const SessionRow row = f.make_session(10, 10);
  MemoryBody body("0123456789");

  CHECK_ERROR(fsrv::upload_chunk(f.ctx, row.id, 0, body, false, -1), 411,
              "LENGTH_REQUIRED");
  CHECK(body.abandon_called);
  CHECK(!body.read_called);
  CHECK(!body.drain_called);
  CHECK_EQ(f.entry_count(row), static_cast<size_t>(0));
}

TEST_CASE(upload_chunk_rejects_a_declared_size_mismatch_and_drains) {
  UploadFixture f;
  const SessionRow row = f.make_session(10, 10);
  MemoryBody body("0123456789");

  CHECK_ERROR(fsrv::upload_chunk(f.ctx, row.id, 0, body, true, 5), 413,
              "CHUNK_SIZE_MISMATCH");
  CHECK(body.drain_called);
  CHECK(!body.read_called);
  CHECK_EQ(f.entry_count(row), static_cast<size_t>(0));
}

TEST_CASE(upload_chunk_rejects_a_short_body_without_leaving_garbage) {
  UploadFixture f;
  const SessionRow row = f.make_session(10, 10);
  MemoryBody body("short");

  CHECK_ERROR(fsrv::upload_chunk(f.ctx, row.id, 0, body, true, 10), 413,
              "CHUNK_SIZE_MISMATCH");
  CHECK(body.read_called);
  CHECK(!body.drain_called);
  CHECK_EQ(f.entry_count(row), static_cast<size_t>(0));
}

TEST_CASE(upload_chunk_rejects_a_long_body_but_consumes_it) {
  UploadFixture f;
  const SessionRow row = f.make_session(10, 10);
  MemoryBody body("0123456789ab");

  CHECK_ERROR(fsrv::upload_chunk(f.ctx, row.id, 0, body, true, 10), 413,
              "CHUNK_SIZE_MISMATCH");
  CHECK(body.read_called);
  CHECK_EQ(body.consumed, static_cast<size_t>(12));
  CHECK_EQ(f.entry_count(row), static_cast<size_t>(0));
}

TEST_CASE(upload_chunk_reports_a_body_read_failure) {
  UploadFixture f;
  const SessionRow row = f.make_session(10, 10);
  MemoryBody body("0123456789");
  body.fail_read = true;

  CHECK_ERROR(fsrv::upload_chunk(f.ctx, row.id, 0, body, true, 10), 400,
              "BODY_READ_FAILED");
  CHECK(body.read_called);
  CHECK_EQ(f.entry_count(row), static_cast<size_t>(0));
}

TEST_CASE(upload_chunk_leaves_the_body_alone_for_an_unknown_session) {
  UploadFixture f;
  MemoryBody body("0123456789");

  CHECK_ERROR(fsrv::upload_chunk(f.ctx, "no-such-session", 0, body, true, 10),
              404, "SESSION_NOT_FOUND");
  CHECK(!body.read_called);
  CHECK(!body.drain_called);
  CHECK(!body.abandon_called);
}

TEST_CASE(upload_chunk_rejects_an_expired_session) {
  UploadFixture f;
  const SessionRow row = f.make_session(10, 10, "out.bin", -10);
  MemoryBody body("0123456789");

  CHECK_ERROR(fsrv::upload_chunk(f.ctx, row.id, 0, body, true, 10), 404,
              "SESSION_EXPIRED");
  CHECK(!body.read_called);
  CHECK(!body.drain_called);
}

TEST_CASE(upload_chunk_rejects_an_out_of_range_index) {
  UploadFixture f;
  const SessionRow row = f.make_session(20, 10);
  MemoryBody body("0123456789");

  CHECK_ERROR(fsrv::upload_chunk(f.ctx, row.id, 2, body, true, 10), 400,
              "CHUNK_INDEX_OUT_OF_RANGE");
  CHECK(!body.read_called);
}

TEST_CASE(upload_complete_accepts_a_zero_byte_session) {
  UploadFixture f;
  const SessionRow row = f.make_session(0, 10, "empty.bin");

  fsrv::upload_complete(f.ctx, row.id);

  const fs::path target = f.ctx.files_root / "empty.bin";
  REQUIRE(fs::exists(target));
  CHECK_EQ(fs::file_size(target), static_cast<uintmax_t>(0));
  CHECK(!fsrv::session_get(row.id).has_value());
  CHECK(!fs::exists(f.chunk_dir(row)));
}

TEST_CASE(upload_complete_merges_chunks_into_the_target) {
  UploadFixture f;
  const SessionRow row = f.make_session(10, 4, "dir/merged.bin");
  // 10 bytes in 4-byte chunks: 4 + 4 + 2.
  const char* parts[3] = {"abcd", "efgh", "ij"};
  const int64_t sizes[3] = {4, 4, 2};
  for (uint32_t i = 0; i < 3; ++i) {
    MemoryBody body(parts[i]);
    fsrv::upload_chunk(f.ctx, row.id, i, body, true, sizes[i]);
  }

  fsrv::upload_complete(f.ctx, row.id);

  const fs::path target = f.ctx.files_root / "dir" / "merged.bin";
  REQUIRE(fs::exists(target));
  CHECK_EQ(testutil::read_bytes(target), std::string("abcdefghij"));
  CHECK(!fsrv::session_get(row.id).has_value());
  CHECK(!fs::exists(f.chunk_dir(row)));
}

TEST_CASE(upload_complete_rejects_an_incomplete_upload) {
  UploadFixture f;
  const SessionRow row = f.make_session(10, 4, "x.bin");
  MemoryBody body("abcd");
  fsrv::upload_chunk(f.ctx, row.id, 0, body, true, 4);

  CHECK_ERROR(fsrv::upload_complete(f.ctx, row.id), 409, "UPLOAD_INCOMPLETE");
  CHECK(fsrv::session_get(row.id).has_value());
  CHECK(fs::exists(f.chunk_dir(row) / "00000000.part"));
}
