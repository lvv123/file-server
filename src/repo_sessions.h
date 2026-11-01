#pragma once

#include <cstdint>
#include <optional>
#include <string>
#include <vector>

namespace fsrv {

struct SessionRow {
  std::string id;
  std::string target_rel_path;
  int64_t total_size = 0;
  uint32_t total_chunks = 0;
  uint32_t chunk_size = 0;
  std::vector<uint8_t> bitmap;
  int64_t created_at = 0;
  int64_t expires_at = 0;
};

void session_insert(const SessionRow& row);
std::optional<SessionRow> session_get(const std::string& id);
void session_update_bitmap(const std::string& id,
                           const std::vector<uint8_t>& bitmap);
void session_delete(const std::string& id);
std::vector<SessionRow> sessions_expired(int64_t now);
int64_t sessions_count_active(int64_t now);

}  // namespace fsrv
