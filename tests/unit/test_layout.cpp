#include <string>

#include "config.h"
#include "layout.h"
#include "test_helpers.h"

namespace fs = std::filesystem;

using fsrv::ConfigError;
using fsrv::ensure_layout_directories;
using fsrv::is_reserved_rel_path;
using fsrv::make_layout;
using fsrv::resolve_serve_root;
using fsrv::RuntimeLayout;
using fsrv::validate_meta_dir_if_exists;

namespace {

template <typename Fn>
bool throws_config_error(Fn&& fn) {
  try {
    fn();
  } catch (const ConfigError&) {
    return true;
  } catch (...) {
    return false;
  }
  return false;
}

}  // namespace

TEST_CASE(layout_resolves_the_current_directory_by_default) {
  const fs::path root = resolve_serve_root("");
  CHECK(root.is_absolute());
  CHECK(fs::is_directory(root));
}

TEST_CASE(layout_resolves_an_explicit_directory) {
  testutil::TempDir dir{"fsrv-layout"};
  CHECK_EQ(resolve_serve_root(dir.path().u8string()), dir.path());
  CHECK(throws_config_error([&] { resolve_serve_root((dir / "nope").u8string()); }));

  testutil::write_bytes(dir / "file.txt", "x");
  CHECK(throws_config_error([&] { resolve_serve_root((dir / "file.txt").u8string()); }));

  const fs::path named = dir / ".file-server";
  fs::create_directories(named);
  CHECK(throws_config_error([&] { resolve_serve_root(named.u8string()); }));
}

TEST_CASE(layout_derives_all_paths) {
  testutil::TempDir dir{"fsrv-layout"};
  const RuntimeLayout layout = make_layout(dir.path());
  CHECK_EQ(layout.serve_root, dir.path());
  CHECK_EQ(layout.meta_root, dir.path() / ".file-server");
  CHECK_EQ(layout.tmp_root, layout.meta_root / "tmp");
  CHECK_EQ(layout.config_path, layout.meta_root / "config.json");
  CHECK_EQ(layout.db_path, layout.meta_root / "meta.db");
}

TEST_CASE(layout_reserved_path_check) {
  CHECK(is_reserved_rel_path(".file-server"));
  CHECK(is_reserved_rel_path(".file-server/meta.db"));
  CHECK(!is_reserved_rel_path(".file-server2"));
  CHECK(!is_reserved_rel_path("a/.file-server"));
  CHECK(!is_reserved_rel_path(""));
}

TEST_CASE(layout_metadata_directory_is_created) {
  testutil::TempDir dir{"fsrv-layout"};
  const RuntimeLayout layout = make_layout(dir.path());
  ensure_layout_directories(layout);
  CHECK(fs::is_directory(layout.meta_root));
  CHECK(fs::is_directory(layout.tmp_root));
  CHECK(!fs::exists(dir.path() / "files"));
}

TEST_CASE(layout_metadata_directory_rejects_foreign_entries) {
  testutil::TempDir dir{"fsrv-layout"};
  const fs::path meta = dir / ".file-server";
  fs::create_directories(meta);
  CHECK(!throws_config_error([&] { validate_meta_dir_if_exists(meta); }));

  testutil::write_bytes(meta / "someone-elses-file.txt", "x");
  CHECK(throws_config_error([&] { validate_meta_dir_if_exists(meta); }));
}

TEST_CASE(layout_metadata_directory_rejects_a_regular_file) {
  testutil::TempDir dir{"fsrv-layout"};
  const fs::path meta = dir / ".file-server";
  testutil::write_bytes(meta, "not a directory");
  CHECK(throws_config_error([&] { validate_meta_dir_if_exists(meta); }));
}

TEST_CASE(layout_metadata_directory_rejects_a_symlink) {
  testutil::TempDir dir{"fsrv-layout"};
  const fs::path target = dir / "elsewhere";
  fs::create_directories(target);
  std::error_code ec;
  fs::create_directory_symlink(target, dir / ".file-server", ec);
  if (ec) return;  // creating symlinks may require privileges on Windows

  CHECK(throws_config_error(
      [&] { validate_meta_dir_if_exists(dir / ".file-server"); }));
}

TEST_CASE(layout_accepts_the_documented_metadata_entries) {
  testutil::TempDir dir{"fsrv-layout"};
  const fs::path meta = dir / ".file-server";
  fs::create_directories(meta / "tmp");
  testutil::write_bytes(meta / "config.json", "{}");
  testutil::write_bytes(meta / "meta.db", "");
  testutil::write_bytes(meta / "meta.db-wal", "");
  testutil::write_bytes(meta / "meta.db-shm", "");
  testutil::write_bytes(meta / "server.lock", "");
  CHECK(!throws_config_error([&] { validate_meta_dir_if_exists(meta); }));
}
