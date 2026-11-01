#pragma once

#include <cstdint>
#include <optional>
#include <string>

namespace fsrv {

// Records exactly one audit row per request. The row is written by the
// destructor so that both success paths and thrown ApiError paths are covered.
// Writing never throws: a broken audit table must not break the request.
class AuditGuard {
 public:
  AuditGuard(std::string client_ip, std::string user_agent, std::string action);
  ~AuditGuard();

  AuditGuard(const AuditGuard&) = delete;
  AuditGuard& operator=(const AuditGuard&) = delete;

  void set_path(std::string rel_path) { rel_path_ = std::move(rel_path); }
  void set_size(std::optional<int64_t> size) { size_ = size; }
  void mark_failed(int status, const std::string& message);
  void skip() { skip_ = true; }
  // Success rows are omitted but failures are still recorded (used for the
  // per-chunk upload endpoint, which would otherwise flood the audit table).
  void skip_success() { skip_success_ = true; }

 private:
  std::string client_ip_;
  std::string user_agent_;
  std::string action_;
  std::string rel_path_;
  std::optional<int64_t> size_;
  std::string error_;
  int64_t started_ms_;
  bool skip_ = false;
  bool skip_success_ = false;
};

// Convenience wrapper used by background maintenance tasks.
void audit_background(const std::string& action, const std::string& result,
                      const std::string& error, int64_t duration_ms);

}  // namespace fsrv
