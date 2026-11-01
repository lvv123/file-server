#include "repo_files.h"

#include <algorithm>
#include <mutex>
#include <unordered_set>

#include "db.h"
#include "error.h"
#include "fsops.h"
#include "paths.h"
#include "util.h"

namespace fsrv {

namespace {

// A full rescan walks the whole tree; two of them running at once would only
// fight over the same rows.
std::mutex g_rescan_mutex;

std::string base_name(const std::string& rel_path) {
  const size_t slash = rel_path.rfind('/');
  return slash == std::string::npos ? rel_path : rel_path.substr(slash + 1);
}

std::string escape_like(const std::string& in) {
  std::string out;
  out.reserve(in.size() + 8);
  for (char c : in) {
    if (c == '\\' || c == '%' || c == '_') out.push_back('\\');
    out.push_back(c);
  }
  return out;
}

}  // namespace

void files_upsert(const std::string& rel_path, bool is_dir, int64_t size,
                  int64_t mtime) {
  Stmt st(conn(), R"sql(
INSERT INTO files(rel_path, name, is_dir, size, mtime)
VALUES(?, ?, ?, ?, ?)
ON CONFLICT(rel_path) DO UPDATE SET
  name=excluded.name, is_dir=excluded.is_dir,
  size=excluded.size, mtime=excluded.mtime
)sql");
  st.bind(1, rel_path)
      .bind(2, base_name(rel_path))
      .bind(3, is_dir)
      .bind(4, size)
      .bind(5, mtime);
  st.run();
}

void files_remove(const std::string& rel_path) {
  Stmt st(conn(), "DELETE FROM files WHERE rel_path = ?");
  st.bind(1, rel_path).run();
}

void files_remove_tree(const std::string& rel_path) {
  with_tx([&] {
    // NOTE: the same value is referenced three times, so it must be bound to
    // the same numbered parameter each time. Binding by position instead is
    // what silently broke descendant removal before.
    Stmt st(conn(), R"sql(
DELETE FROM files
WHERE rel_path = ?1
   OR substr(rel_path, 1, length(?1) + 1) = ?1 || '/'
)sql");
    st.bind(1, rel_path);
    st.run();
  });
}

std::optional<FileRow> files_get(const std::string& rel_path) {
  Stmt st(conn(),
          "SELECT id, rel_path, name, is_dir, size, mtime FROM files "
          "WHERE rel_path = ?");
  st.bind(1, rel_path);
  if (!st.step()) return std::nullopt;
  FileRow row;
  row.id = st.col_int64(0);
  row.rel_path = st.col_text(1);
  row.name = st.col_text(2);
  row.is_dir = st.col_bool(3);
  row.size = st.col_int64(4);
  row.mtime = st.col_int64(5);
  return row;
}

std::vector<FileRow> files_list(const std::string& rel_path) {
  const std::string prefix = rel_path.empty() ? std::string() : rel_path + "/";
  const int64_t plen = static_cast<int64_t>(prefix.size());

  Stmt st(conn(), R"sql(
SELECT id, rel_path, name, is_dir, size, mtime
FROM files
WHERE substr(rel_path, 1, ?) = ?
  AND instr(substr(rel_path, ? + 1), '/') = 0
ORDER BY is_dir DESC, name COLLATE NOCASE ASC
)sql");
  st.bind(1, plen).bind(2, prefix).bind(3, plen);

  std::vector<FileRow> rows;
  while (st.step()) {
    FileRow row;
    row.id = st.col_int64(0);
    row.rel_path = st.col_text(1);
    row.name = st.col_text(2);
    row.is_dir = st.col_bool(3);
    row.size = st.col_int64(4);
    row.mtime = st.col_int64(5);
    rows.push_back(std::move(row));
  }
  return rows;
}

void files_rename_tree(const std::string& from, const std::string& to) {  const int64_t from_len = static_cast<int64_t>(from.size());
  with_tx([&] {
    Stmt st(conn(), R"sql(
UPDATE files
SET rel_path = ? || substr(rel_path, ? + 1),
    name = CASE WHEN length(rel_path) = ? THEN ? ELSE name END
WHERE rel_path = ?
   OR substr(rel_path, 1, ? + 1) = ? || '/'
)sql");
    st.bind(1, to)
        .bind(2, from_len)
        .bind(3, from_len)
        .bind(4, base_name(to))
        .bind(5, from)
        .bind(6, from_len)
        .bind(7, from);
    st.run();
  });
}

void files_make_dirs(const fs::path& root, const std::string& rel_dir) {
  if (rel_dir.empty()) return;
  fs::path cur = root;
  std::string rel_so_far;
  size_t i = 0;
  while (i <= rel_dir.size()) {
    size_t j = rel_dir.find('/', i);
    if (j == std::string::npos) j = rel_dir.size();
    const std::string seg = rel_dir.substr(i, j - i);
    i = j + 1;
    if (seg.empty()) break;
    if (!rel_so_far.empty()) rel_so_far.push_back('/');
    rel_so_far += seg;
    cur /= fs::u8path(seg);

    // An existing *file* in the middle of the path is a user error (409), not
    // an I/O failure: fs::create_directory would surface it as a raw OS error.
    bool is_dir = false;
    int64_t size = 0;
    int64_t mtime = 0;
    const bool exists = stat_path(cur, is_dir, size, mtime);
    if (exists && !is_dir) {
      throw conflict("path component is an existing file: " + rel_so_far,
                     "TARGET_IS_FILE");
    }
    if (!exists) {
      std::error_code ec;
      fs::create_directory(cur, ec);
      if (ec) {
        throw io_error_from("cannot create directory: " + cur.u8string(), ec);
      }
      if (!stat_path(cur, is_dir, size, mtime)) {
        throw io_error_from("cannot stat directory: " + cur.u8string(), ec);
      }
      if (!is_dir) {
        throw conflict("path component is an existing file: " + rel_so_far,
                       "TARGET_IS_FILE");
      }
    }
    files_upsert(rel_so_far, true, 0, mtime);
  }
}

std::vector<SearchHit> files_search(const std::string& q, int limit,
                                    int offset) {
  const std::string pattern = "%" + escape_like(q) + "%";
  Stmt st(conn(), R"sql(
SELECT rel_path, name, size, mtime
FROM files
WHERE is_dir = 0 AND name LIKE ? ESCAPE '\'
ORDER BY name COLLATE NOCASE ASC, rel_path ASC
LIMIT ? OFFSET ?
)sql");
  st.bind(1, pattern).bind(2, limit).bind(3, offset);

  std::vector<SearchHit> hits;
  while (st.step()) {
    SearchHit hit;
    hit.rel_path = st.col_text(0);
    hit.name = st.col_text(1);
    hit.size = st.col_int64(2);
    hit.mtime = st.col_int64(3);
    hits.push_back(std::move(hit));
  }
  return hits;
}

int64_t files_count() {
  Stmt st(conn(), "SELECT COUNT(*) FROM files WHERE is_dir = 0");
  if (!st.step()) return 0;
  return st.col_int64(0);
}

RescanStats files_rescan(const fs::path& root) {
  std::lock_guard<std::mutex> scan_lock(g_rescan_mutex);
  RescanStats stats;

  struct Entry {
    std::string rel;
    bool is_dir;
    int64_t size;
    int64_t mtime;
  };
  std::vector<Entry> entries;

  std::error_code ec;
  fs::recursive_directory_iterator it(root, fs::directory_options::skip_permission_denied, ec);
  const fs::recursive_directory_iterator end;
  for (; it != end; it.increment(ec)) {
    if (ec) {
      log_warn("scan: cannot descend into a directory: " + ec.message());
      ec.clear();
      continue;
    }
    const fs::path& p = it->path();
    std::string rel = p.lexically_relative(root).generic_u8string();
    if (rel.empty() || rel.rfind("..", 0) == 0) continue;

    bool is_dir = false;
    int64_t size = 0;
    int64_t mtime = 0;
    std::error_code stat_ec;
    if (it->is_directory(stat_ec)) {
      is_dir = true;
      if (!stat_path(p, is_dir, size, mtime)) continue;
    } else if (it->is_regular_file(stat_ec)) {
      is_dir = false;
      if (!stat_path(p, is_dir, size, mtime)) continue;
    } else {
      continue;  // symlinks / devices are not indexed
    }
    entries.push_back(Entry{std::move(rel), is_dir, size, mtime});
    ++stats.scanned;
  }

  std::unordered_set<std::string> seen;
  seen.reserve(entries.size() * 2);
  for (const auto& e : entries) seen.insert(e.rel);

  // Upsert in batches so the write lock is never held for long.
  constexpr size_t kBatch = 500;
  for (size_t i = 0; i < entries.size(); i += kBatch) {
    const size_t stop = std::min(entries.size(), i + kBatch);
    with_tx([&] {
      Stmt st(conn(), R"sql(
INSERT INTO files(rel_path, name, is_dir, size, mtime)
VALUES(?, ?, ?, ?, ?)
ON CONFLICT(rel_path) DO UPDATE SET
  name=excluded.name, is_dir=excluded.is_dir,
  size=excluded.size, mtime=excluded.mtime
)sql");
      for (size_t k = i; k < stop; ++k) {
        const Entry& e = entries[k];
        st.bind(1, e.rel)
            .bind(2, base_name(e.rel))
            .bind(3, e.is_dir)
            .bind(4, e.size)
            .bind(5, e.mtime);
        st.run();
        st.reset();
      }
    });
  }
  stats.inserted = static_cast<int64_t>(entries.size());

  // Drop rows whose backing file vanished. Every candidate is re-checked on
  // disk first: an upload that landed between the directory walk and this pass
  // has a row but no entry in `seen`, and must not be deleted.
  std::vector<std::string> stale;
  {
    Stmt st(conn(), "SELECT rel_path FROM files");
    while (st.step()) {
      std::string rel = st.col_text(0);
      if (seen.find(rel) != seen.end()) continue;
      bool is_dir = false;
      int64_t size = 0;
      int64_t mtime = 0;
      if (stat_path(root / fs::u8path(rel), is_dir, size, mtime)) continue;
      stale.push_back(std::move(rel));
    }
  }
  for (size_t i = 0; i < stale.size(); i += kBatch) {
    const size_t stop = std::min(stale.size(), i + kBatch);
    with_tx([&] {
      Stmt st(conn(), "DELETE FROM files WHERE rel_path = ?");
      for (size_t k = i; k < stop; ++k) {
        st.bind(1, stale[k]).run();
        st.reset();
      }
    });
  }
  stats.removed = static_cast<int64_t>(stale.size());
  return stats;
}

}  // namespace fsrv
