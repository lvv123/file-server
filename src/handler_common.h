#pragma once

#include <cstdint>
#include <optional>
#include <string>

#include "audit.h"
#include "error.h"
#include "ext.h"
#include "hpx.h"
#include "util.h"

namespace fsrv {

std::string client_ip_of(const httplib::Request& req);
std::string user_agent_of(const httplib::Request& req);

// Wraps a handler: builds the audit record, converts ApiError into the wire
// format and guarantees exactly one response body.
class RequestScope {
 public:
  RequestScope(const httplib::Request& req, httplib::Response& res,
               std::string action);

  AuditGuard& audit() { return audit_; }

  void ok(Json data = Json::object());
  void ok_status(int status, Json data);

  void set_path(std::string rel_path) { audit_.set_path(std::move(rel_path)); }
  void set_size(std::optional<int64_t> size) { audit_.set_size(size); }

 private:
  httplib::Response& res_;
  AuditGuard audit_;
};

void send_ok(httplib::Response& res, Json data, int status = 200);
void send_error(httplib::Response& res, const ApiError& e);

// Ensures the request body was JSON and is an object.
Json parse_json_body(const httplib::Request& req);

std::string require_string_field(const Json& body, const char* key);
int64_t require_int_field(const Json& body, const char* key);

template <class F>
void handle(const httplib::Request& req, httplib::Response& res,
            const std::string& action, F&& fn) {
  RequestScope scope(req, res, action);
  try {
    fn(scope);
  } catch (const ApiError& e) {
    scope.audit().mark_failed(e.status, e.what());
    send_error(res, e);
  } catch (const std::exception& e) {
    scope.audit().mark_failed(500, e.what());
    log_error(std::string("unhandled exception: ") + e.what());
    send_error(res, internal_error("internal server error"));
  } catch (...) {
    scope.audit().mark_failed(500, "unknown error");
    log_error("unhandled non-standard exception");
    send_error(res, internal_error("internal server error"));
  }
}

}  // namespace fsrv
