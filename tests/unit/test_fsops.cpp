#include <cerrno>
#include <cstdio>
#include <string>

#include "fsops.h"
#include "test_helpers.h"

using fsrv::atomic_replace;
using fsrv::free_bytes;
using fsrv::io_error;
using fsrv::io_error_win32;
using fsrv::probe_same_volume_or_die;
using fsrv::same_volume;
using fsrv::stat_path;
using fsrv::sync_stream;
using fsrv::total_bytes;

namespace fs = std::filesystem;

namespace {

size_t count_entries(const fs::path& dir) {
  size_t n = 0;
  for (const auto& entry : fs::directory_iterator(dir)) {
    (void)entry;
    ++n;
  }
  return n;
}

}  // namespace

TEST_CASE(fsops_stat_path) {
  testutil::TempDir dir{"fsrv-fsops"};
  testutil::write_bytes(dir / "a.bin", "hello");

  bool is_dir = true;
  int64_t size = -1;
  int64_t mtime = -1;
  REQUIRE(stat_path(dir / "a.bin", is_dir, size, mtime));
  CHECK(!is_dir);
  CHECK_EQ(size, static_cast<int64_t>(5));
  CHECK(mtime > 0);

  REQUIRE(stat_path(dir.path(), is_dir, size, mtime));
  CHECK(is_dir);

  CHECK(!stat_path(dir / "missing.bin", is_dir, size, mtime));
}

TEST_CASE(fsops_atomic_replace_overwrites) {
  testutil::TempDir dir{"fsrv-fsops"};
  testutil::write_bytes(dir / "src.tmp", "new-content");
  testutil::write_bytes(dir / "dst.bin", "old-content");

  atomic_replace(dir / "src.tmp", dir / "dst.bin");

  CHECK_EQ(testutil::read_bytes(dir / "dst.bin"), std::string("new-content"));
  CHECK(!fs::exists(dir / "src.tmp"));
}

TEST_CASE(fsops_atomic_replace_creates_target) {
  testutil::TempDir dir{"fsrv-fsops"};
  testutil::write_bytes(dir / "src.tmp", "fresh");
  atomic_replace(dir / "src.tmp", dir / "dst.bin");
  CHECK_EQ(testutil::read_bytes(dir / "dst.bin"), std::string("fresh"));
  CHECK(!fs::exists(dir / "src.tmp"));
}

TEST_CASE(fsops_atomic_replace_missing_source_fails) {
  testutil::TempDir dir{"fsrv-fsops"};
  CHECK_STATUS(atomic_replace(dir / "nope.tmp", dir / "dst.bin"), 404);
}

TEST_CASE(fsops_same_volume) {
  testutil::TempDir dir{"fsrv-fsops"};
  const fs::path files = dir / "files";
  const fs::path tmp = dir / "tmp";
  fs::create_directories(files);
  fs::create_directories(tmp);

  CHECK(same_volume(files, tmp));
  CHECK(same_volume(files, files));
}

TEST_CASE(fsops_probe_same_volume_leaves_no_probe_files) {
  testutil::TempDir dir{"fsrv-fsops"};
  const fs::path files = dir / "files";
  const fs::path tmp = dir / "tmp";
  fs::create_directories(files);
  fs::create_directories(tmp);
  testutil::write_bytes(files / "keep.txt", "keep");

  probe_same_volume_or_die(files, tmp);

  CHECK_EQ(count_entries(files), static_cast<size_t>(1));
  CHECK_EQ(count_entries(tmp), static_cast<size_t>(0));
  CHECK_EQ(testutil::read_bytes(files / "keep.txt"), std::string("keep"));
}

TEST_CASE(fsops_sync_stream_flushes) {
  testutil::TempDir dir{"fsrv-fsops"};
  const fs::path p = dir / "sync.bin";
  std::FILE* f = std::fopen(p.u8string().c_str(), "wb");
  REQUIRE(f != nullptr);
  REQUIRE(std::fwrite("hello", 1, 5, f) == static_cast<size_t>(5));
  sync_stream(f, "test file");
  std::fclose(f);
  CHECK_EQ(testutil::read_bytes(p), std::string("hello"));
}

TEST_CASE(fsops_volume_space_is_queryable) {
  testutil::TempDir dir{"fsrv-fsops"};
  const uint64_t total = total_bytes(dir.path());
  const uint64_t free = free_bytes(dir.path());
  CHECK(total > 0);
  CHECK(free > 0);
  CHECK(free <= total);
}

TEST_CASE(fsops_errno_mapping) {
  CHECK_EQ(io_error("x", ENOENT).status, 404);
  CHECK_EQ(io_error("x", EEXIST).status, 409);
  CHECK_EQ(io_error("x", ENOSPC).status, 507);
  CHECK_EQ(io_error("x", 12345).status, 500);
  CHECK_EQ(io_error("x", 12345).code, std::string("IO_ERROR"));
}

TEST_CASE(fsops_win32_error_mapping) {
  CHECK_EQ(io_error_win32("x", 112).status, 507);  // ERROR_DISK_FULL
  CHECK_EQ(io_error_win32("x", 39).status, 507);   // ERROR_HANDLE_DISK_FULL
  CHECK_EQ(io_error_win32("x", 99999).status, 500);
}
