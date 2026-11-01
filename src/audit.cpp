#include "audit.h"

#include <utility>

#include "repo_audit.h"
#include "util.h"

namespace fsrv {

AuditGuard::AuditGuard(std::string client_ip, std::string user_agent,
                       std::string action)
    : client_ip_(std::move(client_ip)),
      user_agent_(std::move(user_agent)),
      action_(std::move(action)),
      started_ms_(now_ms()) {}

AuditGuard::~AuditGuard() {
  if (skip_) return;
  if (skip_success_ && error_.empty()) return;
  AuditRow row;
  row.ts = now_sec();
  row.client_ip = client_ip_;
  row.action = action_;
  row.rel_path = rel_path_;
  row.size = size_;
  row.result = error_.empty() ? "success" : "fail";
  row.error = error_;
  row.duration_ms = now_ms() - started_ms_;
  if (row.duration_ms < 0) row.duration_ms = 0;
  row.user_agent = user_agent_;
  try {
    audit_insert(row);
  } catch (const std::exception& e) {
    log_error(std::string("audit write failed: ") + e.what());
  } catch (...) {
    log_error("audit write failed: unknown error");
  }
}

void AuditGuard::mark_failed(int status, const std::string& message) {
  error_ = std::to_string(status) + " " + message;
}

void audit_background(const std::string& action, const std::string& result,
                      const std::string& error, int64_t duration_ms) {
  AuditRow row;
  row.ts = now_sec();
  row.client_ip = "127.0.0.1";
  row.action = action;
  row.result = result;
  row.error = error;
  row.duration_ms = duration_ms;
  row.user_agent = "internal";
  try {
    audit_insert(row);
  } catch (const std::exception& e) {
    log_error(std::string("background audit write failed: ") + e.what());
  }
}

}  // namespace fsrv
