#include <cstdint>
#include <stdexcept>
#include <string>
#include <vector>

#include "bitmap.h"
#include "db.h"
#include "repo_audit.h"
#include "repo_files.h"
#include "repo_sessions.h"
#include "test_helpers.h"
#include "util.h"

namespace fs = std::filesystem;

using fsrv::AuditRow;
using fsrv::ChunkBitmap;
using fsrv::FileRow;
using fsrv::SessionRow;

namespace {

// A fresh database per test case, on the thread that runs the assertions.
struct DbFixture {
  testutil::TempDir dir{"fsrv-db"};
  DbFixture() { fsrv::db_init(dir / "meta.db"); }
  ~DbFixture() { fsrv::db_close_current_thread(); }
};

}  // namespace

TEST_CASE(db_pragmas_are_applied) {
  DbFixture f;
  sqlite3* db = fsrv::conn();

  fsrv::Stmt mode(db, "PRAGMA journal_mode;");
  REQUIRE(mode.step());
  CHECK_EQ(fsrv::ascii_lower(mode.col_text(0)), std::string("wal"));

  fsrv::Stmt timeout(db, "PRAGMA busy_timeout;");
  REQUIRE(timeout.step());
  CHECK_EQ(timeout.col_int64(0), static_cast<int64_t>(5000));

  fsrv::Stmt sync(db, "PRAGMA synchronous;");
  REQUIRE(sync.step());
  CHECK_EQ(sync.col_int64(0), static_cast<int64_t>(1));  // NORMAL

  fsrv::Stmt fk(db, "PRAGMA foreign_keys;");
  REQUIRE(fk.step());
  CHECK_EQ(fk.col_int64(0), static_cast<int64_t>(1));

  fsrv::Stmt version(db, "PRAGMA user_version;");
  REQUIRE(version.step());
  CHECK_EQ(version.col_int64(0), static_cast<int64_t>(1));

  CHECK(fsrv::db_used_bytes(db) > 0);
}

TEST_CASE(db_stmt_round_trip) {
  DbFixture f;
  sqlite3* db = fsrv::conn();
  fsrv::exec_sql(db, "CREATE TABLE scratch(id INTEGER PRIMARY KEY, txt TEXT, num INTEGER, data BLOB);");

  {
    fsrv::Stmt ins(db, "INSERT INTO scratch(txt, num, data) VALUES(?, ?, ?);");
    ins.bind(1, std::string_view("hello"));
    ins.bind(2, static_cast<int64_t>(42));
    ins.bind(3, std::vector<uint8_t>{1, 2, 3});
    ins.run();
  }
  CHECK_EQ(fsrv::last_insert_rowid(db), static_cast<int64_t>(1));

  fsrv::Stmt sel(db, "SELECT id, txt, num, data, NULL FROM scratch WHERE num = ?;");
  sel.bind(1, 42);
  REQUIRE(sel.step());
  CHECK_EQ(sel.col_int64(0), static_cast<int64_t>(1));
  CHECK_EQ(sel.col_text(1), std::string("hello"));
  CHECK_EQ(sel.col_int(2), 42);
  const std::vector<uint8_t> blob = sel.col_blob(3);
  REQUIRE(blob.size() == static_cast<size_t>(3));
  CHECK_EQ(blob[2], static_cast<uint8_t>(3));
  CHECK(sel.col_is_null(4));
  CHECK(!sel.step());
}

TEST_CASE(files_upsert_and_get) {
  DbFixture f;
  fsrv::files_upsert("a.txt", false, 5, 100);
  auto row = fsrv::files_get("a.txt");
  REQUIRE(row.has_value());
  CHECK_EQ(row->rel_path, std::string("a.txt"));
  CHECK_EQ(row->name, std::string("a.txt"));
  CHECK(!row->is_dir);
  CHECK_EQ(row->size, static_cast<int64_t>(5));
  CHECK_EQ(row->mtime, static_cast<int64_t>(100));
  CHECK_EQ(fsrv::files_count(), static_cast<int64_t>(1));

  // Upsert updates in place instead of creating a duplicate.
  fsrv::files_upsert("a.txt", false, 9, 200);
  row = fsrv::files_get("a.txt");
  REQUIRE(row.has_value());
  CHECK_EQ(row->size, static_cast<int64_t>(9));
  CHECK_EQ(fsrv::files_count(), static_cast<int64_t>(1));

  CHECK(!fsrv::files_get("missing.txt").has_value());
}

TEST_CASE(files_list_directories_first_then_name_order) {
  DbFixture f;
  fsrv::files_upsert("zeta.txt", false, 1, 1);
  fsrv::files_upsert("Alpha.txt", false, 1, 1);
  fsrv::files_upsert("docs", true, 0, 1);
  fsrv::files_upsert("docs/inner.md", false, 1, 1);
  fsrv::files_upsert("beta", true, 0, 1);

  const std::vector<FileRow> root = fsrv::files_list("");
  REQUIRE(root.size() == static_cast<size_t>(4));
  CHECK(root[0].is_dir);
  CHECK(root[1].is_dir);
  CHECK_EQ(root[0].name, std::string("beta"));
  CHECK_EQ(root[1].name, std::string("docs"));
  CHECK_EQ(root[2].name, std::string("Alpha.txt"));
  CHECK_EQ(root[3].name, std::string("zeta.txt"));

  const std::vector<FileRow> docs = fsrv::files_list("docs");
  REQUIRE(docs.size() == static_cast<size_t>(1));
  CHECK_EQ(docs[0].name, std::string("inner.md"));

  CHECK(fsrv::files_list("nope").empty());
}

TEST_CASE(files_list_does_not_match_sibling_prefixes) {
  DbFixture f;
  fsrv::files_upsert("ab/x.txt", false, 1, 1);
  fsrv::files_upsert("abc/y.txt", false, 1, 1);
  fsrv::files_upsert("ab.txt", false, 1, 1);

  const std::vector<FileRow> ab = fsrv::files_list("ab");
  REQUIRE(ab.size() == static_cast<size_t>(1));
  CHECK_EQ(ab[0].name, std::string("x.txt"));
}

TEST_CASE(files_search_is_case_insensitive_substring) {
  DbFixture f;
  fsrv::files_upsert("docs/Report.PDF", false, 10, 1);
  fsrv::files_upsert("docs/summary.txt", false, 20, 2);
  fsrv::files_upsert("report_folder", true, 0, 3);

  auto hits = fsrv::files_search("report", 100, 0);
  REQUIRE(hits.size() == static_cast<size_t>(1));  // directories are excluded
  CHECK_EQ(hits[0].rel_path, std::string("docs/Report.PDF"));
  CHECK_EQ(hits[0].name, std::string("Report.PDF"));
  CHECK_EQ(hits[0].size, static_cast<int64_t>(10));

  CHECK(fsrv::files_search("zzz", 100, 0).empty());
  CHECK_EQ(fsrv::files_search("", 100, 0).size(), static_cast<size_t>(2));
}

TEST_CASE(files_search_treats_wildcards_literally) {
  DbFixture f;
  fsrv::files_upsert("100% done.txt", false, 1, 1);
  fsrv::files_upsert("a_b.txt", false, 1, 1);
  fsrv::files_upsert("axb.txt", false, 1, 1);
  fsrv::files_upsert("plain.txt", false, 1, 1);

  auto percent = fsrv::files_search("%", 100, 0);
  REQUIRE(percent.size() == static_cast<size_t>(1));
  CHECK_EQ(percent[0].name, std::string("100% done.txt"));

  auto underscore = fsrv::files_search("_", 100, 0);
  REQUIRE(underscore.size() == static_cast<size_t>(1));
  CHECK_EQ(underscore[0].name, std::string("a_b.txt"));
}

TEST_CASE(files_search_paging) {
  DbFixture f;
  for (int i = 0; i < 10; ++i) {
    fsrv::files_upsert("item" + std::to_string(i) + ".txt", false, 1, 1);
  }
  CHECK_EQ(fsrv::files_search("item", 4, 0).size(), static_cast<size_t>(4));
  CHECK_EQ(fsrv::files_search("item", 4, 8).size(), static_cast<size_t>(2));
  CHECK_EQ(fsrv::files_search("item", 4, 20).size(), static_cast<size_t>(0));
}

TEST_CASE(files_rename_tree_moves_descendants) {
  DbFixture f;
  fsrv::files_upsert("docs", true, 0, 1);
  fsrv::files_upsert("docs/a.txt", false, 1, 1);
  fsrv::files_upsert("docs/sub/b.txt", false, 2, 1);
  fsrv::files_upsert("docs2-sibling.txt", false, 3, 1);

  fsrv::files_rename_tree("docs", "archive");

  CHECK(!fsrv::files_get("docs").has_value());
  CHECK(!fsrv::files_get("docs/a.txt").has_value());
  auto moved = fsrv::files_get("archive");
  REQUIRE(moved.has_value());
  CHECK_EQ(moved->name, std::string("archive"));
  CHECK(moved->is_dir);

  auto child = fsrv::files_get("archive/a.txt");
  REQUIRE(child.has_value());
  CHECK_EQ(child->name, std::string("a.txt"));

  auto nested = fsrv::files_get("archive/sub/b.txt");
  REQUIRE(nested.has_value());
  CHECK_EQ(nested->name, std::string("b.txt"));

  // Unrelated rows with a similar prefix are untouched.
  CHECK(fsrv::files_get("docs2-sibling.txt").has_value());
}

TEST_CASE(files_remove_tree_only_removes_descendants) {
  DbFixture f;
  fsrv::files_upsert("docs", true, 0, 1);
  fsrv::files_upsert("docs/a.txt", false, 1, 1);
  fsrv::files_upsert("docs/sub/b.txt", false, 1, 1);
  fsrv::files_upsert("docs-sibling.txt", false, 1, 1);

  fsrv::files_remove_tree("docs");

  CHECK(!fsrv::files_get("docs").has_value());
  CHECK(!fsrv::files_get("docs/a.txt").has_value());
  CHECK(!fsrv::files_get("docs/sub/b.txt").has_value());
  CHECK(fsrv::files_get("docs-sibling.txt").has_value());
  CHECK_EQ(fsrv::files_count(), static_cast<int64_t>(1));

  fsrv::files_remove("docs-sibling.txt");
  CHECK_EQ(fsrv::files_count(), static_cast<int64_t>(0));
}

TEST_CASE(files_make_dirs_creates_every_level) {
  DbFixture f;
  fsrv::files_make_dirs(f.dir.path(), "x/y/z");

  CHECK(fs::is_directory(f.dir / "x"));
  CHECK(fs::is_directory(f.dir / "x/y"));
  CHECK(fs::is_directory(f.dir / "x/y/z"));
  const auto x = fsrv::files_get("x");
  REQUIRE(x.has_value());
  CHECK(x->is_dir);
  const auto xyz = fsrv::files_get("x/y/z");
  REQUIRE(xyz.has_value());
  CHECK(xyz->is_dir);

  // Idempotent: no duplicate rows (files_count() counts files only, so query
  // the table directly here).
  fsrv::files_make_dirs(f.dir.path(), "x/y/z");
  fsrv::Stmt all_rows(fsrv::conn(), "SELECT COUNT(*) FROM files;");
  REQUIRE(all_rows.step());
  CHECK_EQ(all_rows.col_int64(0), static_cast<int64_t>(3));
  CHECK_EQ(fsrv::files_count(), static_cast<int64_t>(0));

  // A file in the way is a conflict.
  testutil::write_bytes(f.dir / "blocker", "x");
  CHECK_ERROR(fsrv::files_make_dirs(f.dir.path(), "blocker/sub"), 409,
              "TARGET_IS_FILE");
}

TEST_CASE(files_make_dirs_empty_is_a_noop) {
  DbFixture f;
  fsrv::files_make_dirs(f.dir.path(), "");
  CHECK_EQ(fsrv::files_count(), static_cast<int64_t>(0));
}

TEST_CASE(files_rescan_adds_and_removes_rows) {
  DbFixture f;
  const fs::path root = f.dir / "root";
  fs::create_directories(root / "sub");
  testutil::write_bytes(root / "a.txt", "aaaa");
  testutil::write_bytes(root / "sub/b.txt", "bb");

  const fsrv::RescanStats stats = fsrv::files_rescan(root);
  CHECK(stats.scanned >= 3);
  CHECK_EQ(stats.removed, static_cast<int64_t>(0));

  auto a = fsrv::files_get("a.txt");
  REQUIRE(a.has_value());
  CHECK_EQ(a->size, static_cast<int64_t>(4));
  auto b = fsrv::files_get("sub/b.txt");
  REQUIRE(b.has_value());
  CHECK_EQ(b->size, static_cast<int64_t>(2));
  CHECK(fsrv::files_get("sub").has_value());

  // Deleting a file on disk drops its row on the next scan.
  fs::remove(root / "a.txt");
  const fsrv::RescanStats second = fsrv::files_rescan(root);
  CHECK_EQ(second.removed, static_cast<int64_t>(1));
  CHECK(!fsrv::files_get("a.txt").has_value());
  CHECK(fsrv::files_get("sub/b.txt").has_value());
}

TEST_CASE(files_rescan_picks_up_size_changes) {
  DbFixture f;
  const fs::path root = f.dir / "root";
  fs::create_directories(root);
  testutil::write_bytes(root / "grow.bin", "1");
  fsrv::files_rescan(root);
  CHECK_EQ(fsrv::files_get("grow.bin")->size, static_cast<int64_t>(1));

  testutil::write_bytes(root / "grow.bin", std::string(5000, 'x'));
  fsrv::files_rescan(root);
  CHECK_EQ(fsrv::files_get("grow.bin")->size, static_cast<int64_t>(5000));
}

TEST_CASE(files_rescan_skips_the_metadata_directory) {
  DbFixture f;
  const fs::path root = f.dir / "root";
  const fs::path meta = root / ".file-server";
  fs::create_directories(meta / "tmp");
  testutil::write_bytes(root / "visible.txt", "ok");
  testutil::write_bytes(meta / "meta.db", "database");
  testutil::write_bytes(meta / "tmp" / "00000000.part", "chunk");

  // Simulate a row left behind by an older version.
  {
    fsrv::Stmt st(fsrv::conn(),
                  "INSERT INTO files(rel_path, name, is_dir, size, mtime) "
                  "VALUES('.file-server/meta.db', 'meta.db', 0, 8, 0)");
    st.run();
  }

  fsrv::files_rescan(root);

  {
    fsrv::Stmt st(fsrv::conn(),
                  "SELECT COUNT(*) FROM files WHERE rel_path = '.file-server' "
                  "OR substr(rel_path, 1, 13) = '.file-server/'");
    REQUIRE(st.step());
    CHECK_EQ(st.col_int64(0), static_cast<int64_t>(0));
  }

  CHECK(!fsrv::files_get(".file-server/meta.db").has_value());
  CHECK_ERROR(fsrv::files_upsert(".file-server/evil.bin", false, 1, 0), 403,
              "RESERVED_PATH");
  CHECK_ERROR(fsrv::files_make_dirs(root, ".file-server/evil"), 403,
              "RESERVED_PATH");
  CHECK_EQ(fsrv::files_count(), static_cast<int64_t>(1));
  const auto root_rows = fsrv::files_list("");
  REQUIRE(root_rows.size() == static_cast<size_t>(1));
  CHECK_EQ(root_rows[0].name, std::string("visible.txt"));
  CHECK(fsrv::files_search("meta", 10, 0).empty());
}

TEST_CASE(sessions_crud) {
  DbFixture f;
  const int64_t now = fsrv::now_sec();

  SessionRow s;
  s.id = "session-1";
  s.target_rel_path = "big/file.bin";
  s.total_size = 100;
  s.total_chunks = 3;
  s.chunk_size = 40;
  s.bitmap = ChunkBitmap(3).blob();
  s.created_at = now;
  s.expires_at = now + 3600;
  fsrv::session_insert(s);

  auto got = fsrv::session_get("session-1");
  REQUIRE(got.has_value());
  CHECK_EQ(got->target_rel_path, std::string("big/file.bin"));
  CHECK_EQ(got->total_size, static_cast<int64_t>(100));
  CHECK_EQ(got->total_chunks, static_cast<uint32_t>(3));
  CHECK_EQ(got->chunk_size, static_cast<uint32_t>(40));
  CHECK_EQ(got->expires_at, now + 3600);

  ChunkBitmap bm = ChunkBitmap::from_blob(got->bitmap, got->total_chunks);
  CHECK_EQ(bm.count(), static_cast<uint32_t>(0));
  bm.set(1);
  fsrv::session_update_bitmap("session-1", bm.blob());
  got = fsrv::session_get("session-1");
  REQUIRE(got.has_value());
  CHECK(ChunkBitmap::from_blob(got->bitmap, 3).test(1));

  CHECK_EQ(fsrv::sessions_count_active(now), static_cast<int64_t>(1));
  CHECK_EQ(fsrv::sessions_expired(now).size(), static_cast<size_t>(0));
  CHECK_EQ(fsrv::sessions_expired(now + 7200).size(), static_cast<size_t>(1));

  fsrv::session_delete("session-1");
  CHECK(!fsrv::session_get("session-1").has_value());
  CHECK_EQ(fsrv::sessions_count_active(now), static_cast<int64_t>(0));
}

TEST_CASE(sessions_expired_is_bounded_and_ordered) {
  DbFixture f;
  const int64_t now = fsrv::now_sec();
  for (int i = 0; i < 5; ++i) {
    SessionRow s;
    s.id = "s" + std::to_string(i);
    s.target_rel_path = "f.bin";
    s.total_size = 10;
    s.total_chunks = 1;
    s.chunk_size = 10;
    s.bitmap = ChunkBitmap(1).blob();
    s.created_at = now;
    s.expires_at = now - 100 + i;  // all expired, different ages
    fsrv::session_insert(s);
  }
  SessionRow live;
  live.id = "live";
  live.target_rel_path = "f.bin";
  live.total_size = 10;
  live.total_chunks = 1;
  live.chunk_size = 10;
  live.bitmap = ChunkBitmap(1).blob();
  live.created_at = now;
  live.expires_at = now + 3600;
  fsrv::session_insert(live);

  const auto expired = fsrv::sessions_expired(now);
  REQUIRE(expired.size() == static_cast<size_t>(5));
  CHECK_EQ(expired.front().id, std::string("s0"));
  CHECK_EQ(expired.back().id, std::string("s4"));
  CHECK_EQ(fsrv::sessions_count_active(now), static_cast<int64_t>(1));
}

TEST_CASE(audit_insert_and_query) {
  DbFixture f;
  const int64_t now = fsrv::now_sec();

  AuditRow fail;
  fail.ts = now - 1;
  fail.client_ip = "10.0.0.2";
  fail.action = "delete";
  fail.rel_path = "b.bin";
  fail.result = "fail";
  fail.error = "not found";
  fail.duration_ms = 1;
  fsrv::audit_insert(fail);

  AuditRow ok;
  ok.ts = now;
  ok.client_ip = "127.0.0.1";
  ok.action = "upload";
  ok.rel_path = "a.bin";
  ok.size = 1234;
  ok.result = "success";
  ok.duration_ms = 7;
  ok.user_agent = "unit-test";
  fsrv::audit_insert(ok);

  CHECK_EQ(fsrv::audit_count(), static_cast<int64_t>(2));
  const auto rows = fsrv::audit_recent(10, 0);
  REQUIRE(rows.size() == static_cast<size_t>(2));
  CHECK_EQ(rows[0].action, std::string("upload"));  // newest first
  CHECK_EQ(rows[0].client_ip, std::string("127.0.0.1"));
  CHECK(rows[0].size.has_value());
  CHECK_EQ(*rows[0].size, static_cast<int64_t>(1234));
  CHECK_EQ(rows[0].user_agent, std::string("unit-test"));
  CHECK_EQ(rows[1].action, std::string("delete"));
  CHECK_EQ(rows[1].result, std::string("fail"));
  CHECK_EQ(rows[1].error, std::string("not found"));
  CHECK(!rows[1].size.has_value());

  CHECK_EQ(fsrv::audit_recent(1, 1).size(), static_cast<size_t>(1));
  CHECK_EQ(fsrv::audit_recent(10, 5).size(), static_cast<size_t>(0));
  CHECK_EQ(fsrv::audit_count_since(now), static_cast<int64_t>(1));
  CHECK_EQ(fsrv::audit_count_since(now - 10), static_cast<int64_t>(2));
}

TEST_CASE(audit_purge_by_age) {
  DbFixture f;
  const int64_t now = fsrv::now_sec();

  AuditRow ancient;
  ancient.ts = now - 100 * 86400;
  ancient.client_ip = "127.0.0.1";
  ancient.action = "download";
  ancient.result = "success";
  fsrv::audit_insert(ancient);

  AuditRow recent;
  recent.ts = now;
  recent.client_ip = "127.0.0.1";
  recent.action = "download";
  recent.result = "success";
  fsrv::audit_insert(recent);

  const fsrv::PurgeStats stats = fsrv::audit_purge(30, 0);
  CHECK_EQ(stats.deleted_by_age, static_cast<int64_t>(1));
  CHECK_EQ(stats.deleted_by_size, static_cast<int64_t>(0));
  CHECK_EQ(fsrv::audit_count(), static_cast<int64_t>(1));

  // Retention disabled (0) removes nothing.
  const fsrv::PurgeStats none = fsrv::audit_purge(0, 0);
  CHECK_EQ(none.deleted_by_age, static_cast<int64_t>(0));
  CHECK_EQ(fsrv::audit_count(), static_cast<int64_t>(1));
}

TEST_CASE(audit_purge_by_size) {
  DbFixture f;
  const int64_t now = fsrv::now_sec();
  const std::string filler(1024, 'x');
  for (int i = 0; i < 2000; ++i) {
    AuditRow row;
    row.ts = now + i;
    row.client_ip = "127.0.0.1";
    row.action = "download";
    row.rel_path = "filler.bin";
    row.result = "fail";
    row.error = filler;
    fsrv::audit_insert(row);
  }
  REQUIRE(fsrv::audit_count() == static_cast<int64_t>(2000));

  const fsrv::PurgeStats stats = fsrv::audit_purge(0, 1);
  CHECK(stats.deleted_by_size > 0);
  CHECK(fsrv::audit_count() < static_cast<int64_t>(2000));

  // The surviving rows are the newest ones.
  const auto rows = fsrv::audit_recent(1, 0);
  REQUIRE(rows.size() == static_cast<size_t>(1));
  CHECK_EQ(rows[0].ts, now + 1999);
}

TEST_CASE(db_transaction_commits_and_rolls_back) {
  DbFixture f;

  fsrv::with_tx([&] { fsrv::files_upsert("committed.txt", false, 1, 1); });
  CHECK(fsrv::files_get("committed.txt").has_value());

  bool threw = false;
  try {
    fsrv::with_tx([&] {
      fsrv::files_upsert("rolled-back.txt", false, 1, 1);
      throw std::runtime_error("boom");
    });
  } catch (const std::runtime_error&) {
    threw = true;
  }
  CHECK(threw);
  CHECK(!fsrv::files_get("rolled-back.txt").has_value());
  CHECK_EQ(fsrv::files_count(), static_cast<int64_t>(1));
}

TEST_CASE(db_nested_transactions_use_savepoints) {
  DbFixture f;

  fsrv::with_tx([&] {
    fsrv::files_upsert("outer.txt", false, 1, 1);
    try {
      fsrv::with_tx([&] {
        fsrv::files_upsert("inner.txt", false, 1, 1);
        throw std::runtime_error("inner failure");
      });
    } catch (const std::runtime_error&) {
    }
  });

  CHECK(fsrv::files_get("outer.txt").has_value());
  CHECK(!fsrv::files_get("inner.txt").has_value());
}

TEST_CASE(db_category_constraint_is_enforced) {
  DbFixture f;
  // rel_path is UNIQUE: re-inserting the same row must update, not duplicate.
  fsrv::files_upsert("unique.txt", false, 1, 1);
  fsrv::files_upsert("unique.txt", false, 2, 2);
  CHECK_EQ(fsrv::files_count(), static_cast<int64_t>(1));
  CHECK_EQ(fsrv::files_get("unique.txt")->size, static_cast<int64_t>(2));
}
