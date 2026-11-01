#pragma once

#include <cstdint>
#include <optional>
#include <string>
#include <vector>

#include "compat.h"

namespace fsrv {

struct FileRow {
  int64_t id = 0;
  std::string rel_path;
  std::string name;
  bool is_dir = false;
  int64_t size = 0;
  int64_t mtime = 0;
};

// Inserts or updates a single index row.
void files_upsert(const std::string& rel_path, bool is_dir, int64_t size,
                  int64_t mtime);

// Removes a single row.
void files_remove(const std::string& rel_path);

// Removes `rel_path` and every row below it.
void files_remove_tree(const std::string& rel_path);

std::optional<FileRow> files_get(const std::string& rel_path);

// Direct children of `rel_path` ("" = root), directories first.
std::vector<FileRow> files_list(const std::string& rel_path);

// Moves `from` (and all descendants) to `to`, renaming the moved node.
void files_rename_tree(const std::string& from, const std::string& to);

// Creates every missing level of `rel_dir` below `root` and indexes it.
void files_make_dirs(const fs::path& root, const std::string& rel_dir);

// Case-insensitive substring search over file names. Escape characters in `q`
// are quoted literally. Returns relative paths of matching files.
struct SearchHit {
  std::string rel_path;
  std::string name;
  int64_t size = 0;
  int64_t mtime = 0;
};
std::vector<SearchHit> files_search(const std::string& q, int limit, int offset);

int64_t files_count();

// Rebuilds the index from the on-disk tree: inserts/updates what exists and
// drops rows whose files disappeared.
struct RescanStats {
  int64_t scanned = 0;
  int64_t inserted = 0;
  int64_t removed = 0;
};
RescanStats files_rescan(const fs::path& root);

}  // namespace fsrv
