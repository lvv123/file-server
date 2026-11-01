#pragma once

#include <cstdint>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

#include <sqlite3.h>

#include "compat.h"

namespace fsrv {

// Opens (and migrates) the database file. Must be called once, before any
// worker thread touches the database.
void db_init(const fs::path& db_file);
void db_close_current_thread();

// Per-thread connection. Every thread gets its own handle; the lifecycle is
// bound to the thread and the four mandatory PRAGMAs are applied on open.
sqlite3* conn();

void exec_sql(sqlite3* db, const std::string& sql);

class Stmt {
 public:
  Stmt(sqlite3* db, const char* sql);
  ~Stmt();
  Stmt(const Stmt&) = delete;
  Stmt& operator=(const Stmt&) = delete;

  Stmt& bind(int index, int64_t value);
  Stmt& bind(int index, int value) { return bind(index, static_cast<int64_t>(value)); }
  Stmt& bind(int index, bool value) {
    return bind(index, static_cast<int64_t>(value ? 1 : 0));
  }
  Stmt& bind(int index, std::string_view value);
  Stmt& bind(int index, const std::vector<uint8_t>& value);
  Stmt& bind_null(int index);

  // true when a row is available; false when the statement is done.
  bool step();
  void run();  // step until done
  void reset();

  int64_t col_int64(int index) const;
  int col_int(int index) const { return static_cast<int>(col_int64(index)); }
  bool col_bool(int index) const { return col_int64(index) != 0; }
  std::string col_text(int index) const;
  std::vector<uint8_t> col_blob(int index) const;
  bool col_is_null(int index) const;

 private:
  sqlite3* db_;
  sqlite3_stmt* stmt_ = nullptr;
};

int64_t last_insert_rowid(sqlite3* db);

// Shared (non-template) transaction nesting state. Keeping this out of with_tx
// matters: a function-local static inside a template is per-instantiation, so
// nesting two different lambdas would start a second real transaction.
namespace detail {
int tx_scope_enter();
void tx_scope_leave();
}  // namespace detail

// Runs `f` inside a transaction (savepoint when nested). Rolls back when `f`
// throws.
template <class F>
void with_tx(F&& f) {
  sqlite3* db = conn();
  const int level = detail::tx_scope_enter();
  try {
    if (level == 1) {
      exec_sql(db, "BEGIN IMMEDIATE");
    } else {
      exec_sql(db, "SAVEPOINT sp" + std::to_string(level));
    }
    f();
    if (level == 1) {
      exec_sql(db, "COMMIT");
    } else {
      exec_sql(db, "RELEASE sp" + std::to_string(level));
    }
  } catch (...) {
    if (level == 1) {
      try {
        exec_sql(db, "ROLLBACK");
      } catch (...) {
      }
    } else {
      try {
        exec_sql(db, "ROLLBACK TO sp" + std::to_string(level));
        exec_sql(db, "RELEASE sp" + std::to_string(level));
      } catch (...) {
      }
    }
    detail::tx_scope_leave();
    throw;
  }
  detail::tx_scope_leave();
}

// Logical (in-use) size of the database file, ignoring free pages.
uint64_t db_used_bytes(sqlite3* db);

}  // namespace fsrv
