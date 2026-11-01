#include "db.h"

#include <chrono>
#include <mutex>
#include <thread>

#include "error.h"
#include "util.h"

namespace fsrv {

namespace {

std::mutex g_init_mutex;
std::string g_db_path;

struct ThreadConn {
  sqlite3* db = nullptr;
  ~ThreadConn() { close(); }

  void close() {
    if (db) {
      sqlite3_close_v2(db);
      db = nullptr;
    }
  }
};

thread_local ThreadConn t_conn;

void apply_pragmas(sqlite3* db) {
  exec_sql(db, "PRAGMA journal_mode=WAL;");
  exec_sql(db, "PRAGMA busy_timeout=5000;");
  exec_sql(db, "PRAGMA synchronous=NORMAL;");
  exec_sql(db, "PRAGMA foreign_keys=ON;");
}

void open_connection(const std::string& path, sqlite3** out) {
  sqlite3* db = nullptr;
  const int rc = sqlite3_open_v2(path.c_str(), &db,
                                 SQLITE_OPEN_READWRITE | SQLITE_OPEN_CREATE |
                                     SQLITE_OPEN_NOMUTEX,
                                 nullptr);
  if (rc != SQLITE_OK) {
    std::string msg = "cannot open database '" + path + "': ";
    msg += db ? sqlite3_errmsg(db) : sqlite3_errstr(rc);
    if (db) sqlite3_close_v2(db);
    throw ApiError(500, "DB_OPEN_FAILED", msg);
  }
  try {
    apply_pragmas(db);
  } catch (...) {
    sqlite3_close_v2(db);
    throw;
  }
  *out = db;
}

}  // namespace

void throw_sqlite_failure(int rc, sqlite3* db, const char* what) {
  std::string msg = what ? what : "sqlite error";
  msg += ": ";
  msg += db ? sqlite3_errmsg(db) : sqlite3_errstr(rc);
  throw ApiError(500, "DB_ERROR", msg);
}

void exec_sql(sqlite3* db, const std::string& sql) {
  char* err = nullptr;
  const int rc = sqlite3_exec(db, sql.c_str(), nullptr, nullptr, &err);
  if (rc != SQLITE_OK) {
    std::string msg = "sqlite exec failed: ";
    msg += err ? err : sqlite3_errstr(rc);
    if (err) sqlite3_free(err);
    throw ApiError(500, "DB_ERROR", msg);
  }
}

namespace {

void create_schema(sqlite3* db) {
  exec_sql(db, R"sql(
CREATE TABLE IF NOT EXISTS files(
  id       INTEGER PRIMARY KEY,
  rel_path TEXT NOT NULL UNIQUE,
  name     TEXT NOT NULL,
  is_dir   INTEGER NOT NULL,
  size     INTEGER NOT NULL DEFAULT 0,
  mtime    INTEGER NOT NULL DEFAULT 0,
  sha256   TEXT NULL
);
CREATE INDEX IF NOT EXISTS idx_files_name ON files(name);
CREATE INDEX IF NOT EXISTS idx_files_dir ON files(is_dir);

CREATE TABLE IF NOT EXISTS upload_sessions(
  id              TEXT PRIMARY KEY,
  target_rel_path TEXT NOT NULL,
  total_size      INTEGER NOT NULL,
  total_chunks    INTEGER NOT NULL,
  chunk_size      INTEGER NOT NULL,
  received_bitmap BLOB,
  created_at      INTEGER NOT NULL,
  expires_at      INTEGER NOT NULL
);
CREATE INDEX IF NOT EXISTS idx_sessions_expires ON upload_sessions(expires_at);

CREATE TABLE IF NOT EXISTS audit_logs(
  id          INTEGER PRIMARY KEY AUTOINCREMENT,
  ts          INTEGER NOT NULL,
  client_ip   TEXT NOT NULL,
  action      TEXT NOT NULL,
  rel_path    TEXT,
  size        INTEGER NULL,
  result      TEXT NOT NULL,
  error       TEXT NULL,
  duration_ms INTEGER NOT NULL DEFAULT 0,
  user_agent  TEXT
);
CREATE INDEX IF NOT EXISTS idx_audit_ts ON audit_logs(ts);
CREATE INDEX IF NOT EXISTS idx_audit_action ON audit_logs(action);
)sql");
  exec_sql(db, "PRAGMA user_version=1;");
}

}  // namespace

void db_init(const fs::path& db_file) {
  std::lock_guard<std::mutex> lk(g_init_mutex);
  t_conn.close();
  g_db_path = db_file.u8string();
  sqlite3* db = nullptr;
  open_connection(g_db_path, &db);
  t_conn.db = db;
  create_schema(db);
}

void db_close_current_thread() {
  t_conn.close();
}

sqlite3* conn() {
  if (t_conn.db) return t_conn.db;
  std::string path;
  {
    std::lock_guard<std::mutex> lk(g_init_mutex);
    path = g_db_path;
  }
  if (path.empty()) {
    throw ApiError(500, "DB_NOT_INITIALISED", "database is not initialised");
  }
  sqlite3* db = nullptr;
  open_connection(path, &db);
  t_conn.db = db;
  return db;
}

Stmt::Stmt(sqlite3* db, const char* sql) : db_(db) {
  const int rc = sqlite3_prepare_v2(db, sql, -1, &stmt_, nullptr);
  if (rc != SQLITE_OK) throw_sqlite_failure(rc, db, "sqlite prepare failed");
}

Stmt::~Stmt() {
  if (stmt_) sqlite3_finalize(stmt_);
}

Stmt& Stmt::bind(int index, int64_t value) {
  sqlite3_bind_int64(stmt_, index, value);
  return *this;
}

Stmt& Stmt::bind(int index, std::string_view value) {
  sqlite3_bind_text(stmt_, index, value.data(),
                    static_cast<int>(value.size()), SQLITE_TRANSIENT);
  return *this;
}

Stmt& Stmt::bind(int index, const std::vector<uint8_t>& value) {
  if (value.empty()) {
    sqlite3_bind_zeroblob(stmt_, index, 0);
  } else {
    sqlite3_bind_blob(stmt_, index, value.data(),
                      static_cast<int>(value.size()), SQLITE_TRANSIENT);
  }
  return *this;
}

Stmt& Stmt::bind_null(int index) {
  sqlite3_bind_null(stmt_, index);
  return *this;
}

bool Stmt::step() {
  for (int attempt = 0;; ++attempt) {
    const int rc = sqlite3_step(stmt_);
    if (rc == SQLITE_ROW) return true;
    if (rc == SQLITE_DONE) return false;
    if (rc == SQLITE_BUSY && attempt < 3) {
      std::this_thread::sleep_for(
          std::chrono::milliseconds(20 * (attempt + 1)));
      continue;
    }
    throw_sqlite_failure(rc, db_, "sqlite step failed");
  }
}

void Stmt::run() {
  while (step()) {
  }
}

void Stmt::reset() {
  sqlite3_reset(stmt_);
  sqlite3_clear_bindings(stmt_);
}

int64_t Stmt::col_int64(int index) const {
  return sqlite3_column_int64(stmt_, index);
}

std::string Stmt::col_text(int index) const {
  const unsigned char* p = sqlite3_column_text(stmt_, index);
  if (!p) return {};
  const int n = sqlite3_column_bytes(stmt_, index);
  return std::string(reinterpret_cast<const char*>(p),
                     static_cast<size_t>(n));
}

std::vector<uint8_t> Stmt::col_blob(int index) const {
  const void* p = sqlite3_column_blob(stmt_, index);
  const int n = sqlite3_column_bytes(stmt_, index);
  if (!p || n <= 0) return {};
  const auto* b = static_cast<const uint8_t*>(p);
  return std::vector<uint8_t>(b, b + n);
}

bool Stmt::col_is_null(int index) const {
  return sqlite3_column_type(stmt_, index) == SQLITE_NULL;
}

int64_t last_insert_rowid(sqlite3* db) { return sqlite3_last_insert_rowid(db); }

namespace detail {

namespace {
thread_local int t_tx_depth = 0;
}  // namespace

int tx_scope_enter() { return ++t_tx_depth; }

void tx_scope_leave() {
  if (t_tx_depth > 0) --t_tx_depth;
}

}  // namespace detail

uint64_t db_used_bytes(sqlite3* db) {
  Stmt st(db, "PRAGMA page_count;");
  int64_t page_count = 0;
  if (st.step()) page_count = st.col_int64(0);
  st.reset();
  Stmt st2(db, "PRAGMA freelist_count;");
  int64_t freelist = 0;
  if (st2.step()) freelist = st2.col_int64(0);
  int64_t page_size = 4096;
  Stmt st3(db, "PRAGMA page_size;");
  if (st3.step()) page_size = st3.col_int64(0);
  int64_t used = page_count - freelist;
  if (used < 0) used = 0;
  return static_cast<uint64_t>(used) * static_cast<uint64_t>(page_size);
}

}  // namespace fsrv
