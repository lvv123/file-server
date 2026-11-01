#include "repo_sessions.h"

#include "db.h"

namespace fsrv {

void session_insert(const SessionRow& row) {
  Stmt st(conn(), R"sql(
INSERT INTO upload_sessions
  (id, target_rel_path, total_size, total_chunks, chunk_size,
   received_bitmap, created_at, expires_at)
VALUES(?, ?, ?, ?, ?, ?, ?, ?)
)sql");
  st.bind(1, row.id)
      .bind(2, row.target_rel_path)
      .bind(3, row.total_size)
      .bind(4, static_cast<int64_t>(row.total_chunks))
      .bind(5, static_cast<int64_t>(row.chunk_size))
      .bind(6, row.bitmap)
      .bind(7, row.created_at)
      .bind(8, row.expires_at);
  st.run();
}

namespace {

SessionRow read_row(const Stmt& st) {
  SessionRow row;
  row.id = st.col_text(0);
  row.target_rel_path = st.col_text(1);
  row.total_size = st.col_int64(2);
  row.total_chunks = static_cast<uint32_t>(st.col_int64(3));
  row.chunk_size = static_cast<uint32_t>(st.col_int64(4));
  row.bitmap = st.col_blob(5);
  row.created_at = st.col_int64(6);
  row.expires_at = st.col_int64(7);
  return row;
}

constexpr const char* kSelectColumns =
    "SELECT id, target_rel_path, total_size, total_chunks, chunk_size, "
    "received_bitmap, created_at, expires_at FROM upload_sessions ";

}  // namespace

std::optional<SessionRow> session_get(const std::string& id) {
  std::string sql = kSelectColumns;
  sql += "WHERE id = ?";
  Stmt st(conn(), sql.c_str());
  st.bind(1, id);
  if (!st.step()) return std::nullopt;
  return read_row(st);
}

void session_update_bitmap(const std::string& id,
                           const std::vector<uint8_t>& bitmap) {
  Stmt st(conn(), "UPDATE upload_sessions SET received_bitmap = ? WHERE id = ?");
  st.bind(1, bitmap).bind(2, id);
  st.run();
}

void session_delete(const std::string& id) {
  Stmt st(conn(), "DELETE FROM upload_sessions WHERE id = ?");
  st.bind(1, id).run();
}

std::vector<SessionRow> sessions_expired(int64_t now) {
  std::string sql = kSelectColumns;
  sql += "WHERE expires_at <= ? ORDER BY expires_at ASC LIMIT 500";
  Stmt st(conn(), sql.c_str());
  st.bind(1, now);
  std::vector<SessionRow> rows;
  while (st.step()) rows.push_back(read_row(st));
  return rows;
}

int64_t sessions_count_active(int64_t now) {
  Stmt st(conn(), "SELECT COUNT(*) FROM upload_sessions WHERE expires_at > ?");
  st.bind(1, now);
  if (!st.step()) return 0;
  return st.col_int64(0);
}

}  // namespace fsrv
