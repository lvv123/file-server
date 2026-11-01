#include "error.h"
#include "fsops.h"
#include "handler_routes.h"
#include "repo_audit.h"
#include "repo_files.h"
#include "repo_sessions.h"
#include "upload.h"
#include "util.h"

namespace fsrv {

namespace {

// Strips the IPv4-mapped IPv6 prefix so "::ffff:127.0.0.1" matches "127.0.0.1".
std::string normalize_ip(std::string ip) {
  if (ascii_istarts_with(ip, "::ffff:")) ip = ip.substr(7);
  return ip;
}

}  // namespace

bool is_admin_client(const Config& cfg, const std::string& ip) {
  const std::string candidate = normalize_ip(ip);
  for (const auto& allowed : cfg.admin_allow_ips) {
    if (candidate == normalize_ip(allowed)) return true;
  }
  return false;
}

void handle_admin_status(const AppContext& ctx, const httplib::Request& req,
                         httplib::Response& res) {
  handle(req, res, "admin_status", [&](RequestScope& scope) {
    if (!is_admin_client(ctx.cfg, client_ip_of(req))) {
      throw forbidden("the admin API is only reachable from an allowed address",
                      "ADMIN_FORBIDDEN");
    }
    scope.set_path("/");

    const int64_t now = now_sec();
    const uint64_t total = total_bytes(ctx.data_root);
    const uint64_t free = free_bytes(ctx.data_root);

    Json data;
    data["data_root"] = ctx.data_root.u8string();
    data["files_root"] = ctx.files_root.u8string();
    data["disk"] = Json{{"total", total},
                        {"free", free},
                        {"used", total > free ? total - free : 0},
                        {"total_text", human_size(total)},
                        {"free_text", human_size(free)}};
    data["file_count"] = files_count();
    data["active_sessions"] = sessions_count_active(now);
    data["recent_audits"] = audit_count_since(now - 86400);
    data["audit_total"] = audit_count();
    data["uptime_seconds"] = ctx.started_at > 0 ? now - ctx.started_at : 0;
    data["chunk_size_mb"] = ctx.cfg.chunk_size_mb;
    data["version"] = "1.0.0";
    scope.ok(std::move(data));
  });
}

void handle_admin_audit(const AppContext& ctx, const httplib::Request& req,
                        httplib::Response& res) {
  handle(req, res, "admin_audit", [&](RequestScope& scope) {
    if (!is_admin_client(ctx.cfg, client_ip_of(req))) {
      throw forbidden("the admin API is only reachable from an allowed address",
                      "ADMIN_FORBIDDEN");
    }

    int limit = 100;
    const std::string limit_param = req.get_param_value("limit");
    if (!limit_param.empty()) {
      try {
        limit = std::stoi(limit_param);
      } catch (...) {
        throw bad_request("limit must be an integer", "INVALID_LIMIT");
      }
    }
    if (limit < 1) limit = 1;
    if (limit > 1000) limit = 1000;

    int offset = 0;
    const std::string offset_param = req.get_param_value("offset");
    if (!offset_param.empty()) {
      try {
        offset = std::stoi(offset_param);
      } catch (...) {
        throw bad_request("offset must be an integer", "INVALID_OFFSET");
      }
    }
    if (offset < 0) offset = 0;

    Json rows = Json::array();
    for (const auto& row : audit_recent(limit, offset)) {
      Json item;
      item["id"] = row.id;
      item["ts"] = row.ts;
      item["time"] = format_time_iso(row.ts);
      item["client_ip"] = row.client_ip;
      item["action"] = row.action;
      // Audit rows store the path relative to the data root; present it the same
      // way the file APIs do, with a leading slash.
      std::string display_path = row.rel_path;
      if (display_path.empty()) {
        display_path = "/";
      } else if (display_path.front() != '/') {
        display_path.insert(display_path.begin(), '/');
      }
      item["path"] = std::move(display_path);
      if (row.size.has_value()) {
        item["size"] = *row.size;
      } else {
        item["size"] = nullptr;
      }
      item["result"] = row.result;
      item["error"] = row.error;
      item["duration_ms"] = row.duration_ms;
      item["user_agent"] = row.user_agent;
      rows.push_back(std::move(item));
    }

    Json data;
    data["limit"] = limit;
    data["offset"] = offset;
    data["total"] = audit_count();
    data["records"] = std::move(rows);
    scope.ok(std::move(data));
  });
}

void handle_admin_delete(const AppContext& ctx, const httplib::Request& req,
                         httplib::Response& res) {
  handle(req, res, "admin_delete", [&](RequestScope& scope) {
    if (!is_admin_client(ctx.cfg, client_ip_of(req))) {
      throw forbidden("the admin API is only reachable from an allowed address",
                      "ADMIN_FORBIDDEN");
    }
    perform_delete(ctx, req, res, scope);
  });
}

}  // namespace fsrv
