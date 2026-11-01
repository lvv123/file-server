#include "handler_common.h"

#include "error.h"
#include "util.h"

namespace fsrv {

std::string client_ip_of(const httplib::Request& req) {
  // Deliberately the TCP peer address, never X-Forwarded-For: the admin API
  // relies on this for its local-only rule, and a client-supplied header must
  // not be able to grant itself access.
  if (req.remote_addr.empty()) return "unknown";
  return req.remote_addr;
}

std::string user_agent_of(const httplib::Request& req) {
  if (!req.has_header("User-Agent")) return {};
  return req.get_header_value("User-Agent");
}

RequestScope::RequestScope(const httplib::Request& req, httplib::Response& res,
                           std::string action)
    : res_(res),
      audit_(client_ip_of(req), user_agent_of(req), std::move(action)) {}

void RequestScope::ok(Json data) { send_ok(res_, std::move(data)); }

void RequestScope::ok_status(int status, Json data) {
  send_ok(res_, std::move(data), status);
}

void send_ok(httplib::Response& res, Json data, int status) {
  Json body;
  body["ok"] = true;
  body["data"] = std::move(data);
  res.status = status;
  res.set_content(body.dump(), "application/json; charset=utf-8");
}

void send_error(httplib::Response& res, const ApiError& e) {
  Json body;
  body["ok"] = false;
  body["error"] = Json{{"code", e.code}, {"message", e.what()}};
  res.status = e.status;
  res.set_content(body.dump(), "application/json; charset=utf-8");
}

Json parse_json_body(const httplib::Request& req) {
  if (req.body.empty()) {
    throw bad_request("request body is required", "MISSING_BODY");
  }
  Json body = Json::parse(req.body, nullptr, false);
  if (body.is_discarded()) {
    throw bad_request("request body is not valid JSON", "INVALID_JSON");
  }
  if (!body.is_object()) {
    throw bad_request("request body must be a JSON object", "INVALID_JSON");
  }
  return body;
}

std::string require_string_field(const Json& body, const char* key) {
  if (!body.contains(key)) {
    throw bad_request(std::string("missing required field: ") + key,
                      "MISSING_FIELD");
  }
  if (!body[key].is_string()) {
    throw bad_request(std::string("field must be a string: ") + key,
                      "INVALID_FIELD");
  }
  return body[key].get<std::string>();
}

int64_t require_int_field(const Json& body, const char* key) {
  if (!body.contains(key)) {
    throw bad_request(std::string("missing required field: ") + key,
                      "MISSING_FIELD");
  }
  const Json& v = body[key];
  if (v.is_number_unsigned()) {
    const uint64_t u = v.get<uint64_t>();
    if (u > static_cast<uint64_t>(INT64_MAX)) {
      throw bad_request(std::string("field is out of range: ") + key,
                        "INVALID_FIELD");
    }
    return static_cast<int64_t>(u);
  }
  if (!v.is_number_integer()) {
    throw bad_request(std::string("field must be an integer: ") + key,
                      "INVALID_FIELD");
  }
  return v.get<int64_t>();
}

}  // namespace fsrv
