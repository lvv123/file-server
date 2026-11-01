#include "repo_audit.h"

#include <algorithm>
#include <vector>

#include "db.h"
#include "error.h"
#include "util.h"

namespace fsrv {

namespace {

AuditRow read_row(const Stmt& st) {
  AuditRow row;
  row.id = st.col_int64(0);
  row.ts = st.col_int64(1);
  row.client_ip = st.col_text(2);
  row.action = st.col_text(3);
  row.rel_path = st.col_text(4);
  if (st.col_is_null(5)) {
    row.size = std::nullopt;
  } else {
    row.size = st.col_int64(5);
  }
  row.result = st.col_text(6);
  row.error = st.col_text(7);
  row.duration_ms = st.col_int64(8);
  row.user_agent = st.col_text(9);
  return row;
}

constexpr const char* kSelectColumns =
    "SELECT id, ts, client_ip, action, rel_path, size, result, error, "
    "duration_ms, user_agent FROM audit_logs ";

}  // namespace

void audit_insert(const AuditRow& row) {
  Stmt st(conn(), R"sql(
INSERT INTO audit_logs(ts, client_ip, action, rel_path, size, result, error, duration_ms, user_agent)
VALUES(?, ?, ?, ?, ?, ?, ?, ?, ?)
)sql");
  st.bind(1, row.ts)
      .bind(2, row.client_ip)
      .bind(3, row.action)
      .bind(4, row.rel_path);
  if (row.size.has_value()) {
    st.bind(5, *row.size);
  } else {
    st.bind_null(5);
  }
  st.bind(6, row.result).bind(7, row.error).bind(8, row.duration_ms).bind(9, row.user_agent);
  st.run();
}

std::vector<AuditRow> audit_recent(int limit, int offset) {
  std::string sql = kSelectColumns;
  sql += "ORDER BY id DESC LIMIT ? OFFSET ?";
  Stmt st(conn(), sql.c_str());
  st.bind(1, limit).bind(2, offset);
  std::vector<AuditRow> rows;
  while (st.step()) rows.push_back(read_row(st));
  return rows;
}

int64_t audit_count() {
  Stmt st(conn(), "SELECT COUNT(*) FROM audit_logs");
  if (!st.step()) return 0;
  return st.col_int64(0);
}

int64_t audit_count_since(int64_t ts) {
  Stmt st(conn(), "SELECT COUNT(*) FROM audit_logs WHERE ts >= ?");
  st.bind(1, ts);
  if (!st.step()) return 0;
  return st.col_int64(0);
}

PurgeStats audit_purge(int retention_days, int max_size_mb) {
  PurgeStats stats;
  const int64_t now = now_sec();

  if (retention_days > 0) {
    const int64_t cutoff = now - static_cast<int64_t>(retention_days) * 86400;
    with_tx([&] {
      Stmt st(conn(), "DELETE FROM audit_logs WHERE ts < ?");
      st.bind(1, cutoff).run();
      stats.deleted_by_age = sqlite3_changes(conn());
      if (stats.deleted_by_age < 0) stats.deleted_by_age = 0;
    });
  }

  if (max_size_mb > 0) {
    const uint64_t cap = static_cast<uint64_t>(max_size_mb) * 1024ULL * 1024ULL;
    // Delete small enough batches that a single pass cannot wipe the table: the
    // old code deleted 2000 rows at a time, which for a small cap removed
    // everything including the newest records.
    constexpr int kBatch = 200;
    if (db_used_bytes(conn()) > cap) {
      try {
        exec_sql(conn(), "PRAGMA wal_checkpoint(TRUNCATE);");
      } catch (...) {
      }
    }
    while (db_used_bytes(conn()) > cap) {
      int64_t deleted = 0;
      with_tx([&] {
        Stmt st(conn(),
                "DELETE FROM audit_logs WHERE id IN "
                "(SELECT id FROM audit_logs ORDER BY id ASC LIMIT ?)");
        st.bind(1, kBatch).run();
        deleted = sqlite3_changes(conn());
        if (deleted < 0) deleted = 0;
      });
      if (deleted == 0) break;  // table is empty but the file cannot shrink
      stats.deleted_by_size += deleted;
    }
  }

  if (stats.deleted_by_age > 0 || stats.deleted_by_size > 0) {
    try {
      exec_sql(conn(), "PRAGMA wal_checkpoint(TRUNCATE);");
    } catch (...) {
    }
  }
  return stats;
}

}  // namespace fsrv
