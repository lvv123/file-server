#include <string>

#include "paths.h"
#include "test_helpers.h"

namespace fs = std::filesystem;

using fsrv::is_windows_reserved_name;
using fsrv::PathGuard;

namespace {

// PathGuard needs an existing directory, so every test builds a throw-away one.
struct GuardFixture {
  testutil::TempDir dir{"fsrv-paths"};
  PathGuard guard = PathGuard::make(dir.path());
};

}  // namespace

TEST_CASE(paths_normalize_simple_cases) {
  GuardFixture f;
  CHECK_EQ(f.guard.normalize(""), std::string(""));
  CHECK_EQ(f.guard.normalize("/"), std::string(""));
  CHECK_EQ(f.guard.normalize("a"), std::string("a"));
  CHECK_EQ(f.guard.normalize("/a"), std::string("a"));
  CHECK_EQ(f.guard.normalize("/a/b"), std::string("a/b"));
  CHECK_EQ(f.guard.normalize("/a/b/"), std::string("a/b"));
  CHECK_EQ(f.guard.normalize("/a//b///c"), std::string("a/b/c"));
  CHECK_EQ(f.guard.normalize("/a/./b"), std::string("a/b"));
  CHECK_EQ(f.guard.normalize("/a b/c d.txt"), std::string("a b/c d.txt"));
  CHECK_EQ(f.guard.normalize("/%E4%B8%AD%E6%96%87/f.txt"),
           std::string("\xE4\xB8\xAD\xE6\x96\x87/f.txt"));
}

TEST_CASE(paths_backslashes_are_separators) {
  GuardFixture f;
  CHECK_EQ(f.guard.normalize("a\\b\\c"), std::string("a/b/c"));
  CHECK_EQ(f.guard.normalize("\\a\\b"), std::string("a/b"));
}

TEST_CASE(paths_traversal_is_rejected) {
  GuardFixture f;
  CHECK_ERROR(f.guard.normalize(".."), 403, "PATH_TRAVERSAL");
  CHECK_ERROR(f.guard.normalize("../etc/passwd"), 403, "PATH_TRAVERSAL");
  CHECK_ERROR(f.guard.normalize("/a/../../b"), 403, "PATH_TRAVERSAL");
  CHECK_ERROR(f.guard.normalize("a/.."), 403, "PATH_TRAVERSAL");
  CHECK_ERROR(f.guard.normalize("..\\..\\Windows"), 403, "PATH_TRAVERSAL");
  CHECK_ERROR(f.guard.normalize("/..%5C..%5CWindows"), 403, "PATH_TRAVERSAL");
  CHECK_ERROR(f.guard.normalize("/%2e%2e/etc"), 403, "PATH_TRAVERSAL");
  CHECK_ERROR(f.guard.normalize("/a/%2E%2E%2Fb"), 403, "PATH_TRAVERSAL");
  CHECK_ERROR(f.guard.normalize("C:\\Windows"), 400, "INVALID_PATH");
}

TEST_CASE(paths_unc_and_device_paths_are_rejected) {
  GuardFixture f;
  CHECK_ERROR(f.guard.normalize("//server/share/file"), 403, "PATH_TRAVERSAL");
  CHECK_ERROR(f.guard.normalize("\\\\server\\share"), 403, "PATH_TRAVERSAL");
  CHECK_ERROR(f.guard.normalize("//./C:/Windows"), 403, "PATH_TRAVERSAL");
}

TEST_CASE(paths_encoding_errors) {
  GuardFixture f;
  CHECK_ERROR(f.guard.normalize("/a/%zz"), 400, "INVALID_ENCODING");
  CHECK_ERROR(f.guard.normalize("/a/%2"), 400, "INVALID_ENCODING");
  CHECK_ERROR(f.guard.normalize("/a/%GF"), 400, "INVALID_ENCODING");
}

TEST_CASE(paths_illegal_characters) {
  GuardFixture f;
  CHECK_ERROR(f.guard.normalize("/a%00b"), 400, "INVALID_PATH");
  CHECK_ERROR(f.guard.normalize("/a:b"), 400, "INVALID_PATH");
  CHECK_ERROR(f.guard.normalize("/a?b"), 400, "INVALID_PATH");
  CHECK_ERROR(f.guard.normalize("/a*b"), 400, "INVALID_PATH");
  CHECK_ERROR(f.guard.normalize("/a|b"), 400, "INVALID_PATH");
  CHECK_ERROR(f.guard.normalize("/a<b>"), 400, "INVALID_PATH");
  CHECK_ERROR(f.guard.normalize("/a%22b"), 400, "INVALID_PATH");
  CHECK_ERROR(f.guard.normalize("/a%09b"), 400, "INVALID_PATH");
}

TEST_CASE(paths_rejects_trailing_dot_or_space) {
  GuardFixture f;
  CHECK_ERROR(f.guard.normalize("/a./b"), 400, "INVALID_PATH");
  CHECK_ERROR(f.guard.normalize("/a /b"), 400, "INVALID_PATH");
  CHECK_ERROR(f.guard.normalize("/a."), 400, "INVALID_PATH");
  CHECK_ERROR(f.guard.normalize("/a "), 400, "INVALID_PATH");
  CHECK_ERROR(f.guard.normalize("/a/.../b"), 400, "INVALID_PATH");
  CHECK_ERROR(f.guard.normalize("/ leading"), 400, "INVALID_PATH");
}

TEST_CASE(paths_rejects_invalid_utf8) {
  GuardFixture f;
  CHECK_ERROR(f.guard.normalize("/%FF"), 400, "INVALID_PATH");
  CHECK_ERROR(f.guard.normalize("/%C0%AF"), 400, "INVALID_PATH");
  CHECK_ERROR(f.guard.normalize("/%ED%A0%80"), 400, "INVALID_PATH");
}

TEST_CASE(paths_segment_length_limit) {
  GuardFixture f;
  CHECK_EQ(f.guard.normalize("/" + std::string(255, 'a')).size(),
           static_cast<size_t>(255));
  CHECK_STATUS(f.guard.normalize("/" + std::string(256, 'a')), 400);
}

TEST_CASE(paths_windows_reserved_names) {
  GuardFixture f;
  CHECK(is_windows_reserved_name("CON"));
  CHECK(is_windows_reserved_name("con"));
  CHECK(is_windows_reserved_name("Con.txt"));
  CHECK(is_windows_reserved_name("NUL"));
  CHECK(is_windows_reserved_name("aux.log"));
  CHECK(is_windows_reserved_name("COM1"));
  CHECK(is_windows_reserved_name("lpt9.tar"));
  CHECK(!is_windows_reserved_name("COM0"));
  CHECK(!is_windows_reserved_name("COM10"));
  CHECK(!is_windows_reserved_name("console.txt"));
  CHECK(!is_windows_reserved_name("nulify"));

  CHECK_STATUS(f.guard.normalize("/CON"), 400);
  CHECK_STATUS(f.guard.normalize("/dir/con.txt"), 400);
}

TEST_CASE(paths_resolve_stays_inside_root) {
  GuardFixture f;
  CHECK(f.guard.resolve("/") == f.dir.path());
  CHECK(f.guard.resolve("/a/b") == f.dir.path() / "a" / "b");
  CHECK(f.guard.resolve("") == f.dir.path());
  CHECK_STATUS(f.guard.resolve("/../escape"), 403);

  // A symlink/junction that points outside the root is caught by the canonical
  // containment check. Creating one needs privileges on Windows, so only the
  // lexical fallback is exercised here.
  const fs::path inside = f.dir.path() / "a" / "b";
  CHECK(PathGuard::is_within(inside, f.dir.path()));
}

TEST_CASE(paths_to_rel) {
  GuardFixture f;
  CHECK_EQ(f.guard.to_rel(f.dir.path()), std::string(""));
  CHECK_EQ(f.guard.to_rel(f.dir.path() / "a"), std::string("a"));
  CHECK_EQ(f.guard.to_rel(f.dir.path() / "a" / "b" / "c.txt"),
           std::string("a/b/c.txt"));
  CHECK_STATUS(f.guard.to_rel(f.dir.path() / ".." / "outside"), 500);
}

TEST_CASE(paths_is_within_component_wise) {
  const fs::path root = testutil::fs::absolute("base-root");
  CHECK(PathGuard::is_within(root, root));
  CHECK(PathGuard::is_within(root / "a", root));
  CHECK(PathGuard::is_within(root / "a" / "b", root));
  CHECK(!PathGuard::is_within(root / "ab", root / "a"));
  CHECK(!PathGuard::is_within(root, root / "a"));
  CHECK(!PathGuard::is_within(root / "other", root / "a"));
}

TEST_CASE(paths_make_requires_existing_directory) {
  testutil::TempDir dir{"fsrv-paths-missing"};
  CHECK_STATUS(PathGuard::make(dir / "nope"), 500);
  testutil::write_bytes(dir / "file.txt", "x");
  CHECK_STATUS(PathGuard::make(dir / "file.txt"), 500);
}
