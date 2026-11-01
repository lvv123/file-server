#pragma once

#include <cstdint>
#include <optional>
#include <string>
#include <vector>

namespace fsrv {

struct AuditRow {
  int64_t id = 0;
  int64_t ts = 0;
  std::string client_ip;
  std::string action;
  std::string rel_path;
  std::optional<int64_t> size;
  std::string result;
  std::string error;
  int64_t duration_ms = 0;
  std::string user_agent;
};

void audit_insert(const AuditRow& row);

// Newest first.
std::vector<AuditRow> audit_recent(int limit, int offset);

int64_t audit_count();
int64_t audit_count_since(int64_t ts);

struct PurgeStats {
  int64_t deleted_by_age = 0;
  int64_t deleted_by_size = 0;
};

// Applies the retention policy. `retention_days` <= 0 disables the age rule,
// `max_size_mb` <= 0 disables the capacity rule.
PurgeStats audit_purge(int retention_days, int max_size_mb);

}  // namespace fsrv
