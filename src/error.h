#pragma once

#include <stdexcept>
#include <string>
#include <utility>

namespace fsrv {

// Every user-visible failure is expressed as an ApiError carrying the HTTP
// status, a stable machine-readable code and a human readable message.
struct ApiError : std::runtime_error {
  int status;
  std::string code;

  ApiError(int st, std::string c, std::string msg)
      : std::runtime_error(std::move(msg)), status(st), code(std::move(c)) {}
};

inline ApiError bad_request(std::string msg, std::string code = "BAD_REQUEST") {
  return ApiError(400, std::move(code), std::move(msg));
}
inline ApiError length_required(std::string msg = "Content-Length is required") {
  return ApiError(411, "LENGTH_REQUIRED", std::move(msg));
}
inline ApiError forbidden(std::string msg, std::string code = "FORBIDDEN") {
  return ApiError(403, std::move(code), std::move(msg));
}
inline ApiError path_traversal(std::string msg = "path escapes the server root") {
  return ApiError(403, "PATH_TRAVERSAL", std::move(msg));
}
inline ApiError not_found(std::string msg, std::string code = "NOT_FOUND") {
  return ApiError(404, std::move(code), std::move(msg));
}
inline ApiError conflict(std::string msg, std::string code = "CONFLICT") {
  return ApiError(409, std::move(code), std::move(msg));
}
inline ApiError too_large(std::string msg, std::string code = "PAYLOAD_TOO_LARGE") {
  return ApiError(413, std::move(code), std::move(msg));
}
inline ApiError range_not_satisfiable(std::string msg = "range not satisfiable") {
  return ApiError(416, "RANGE_NOT_SATISFIABLE", std::move(msg));
}
inline ApiError disk_full(std::string msg = "no space left on device") {
  return ApiError(507, "INSUFFICIENT_STORAGE", std::move(msg));
}
inline ApiError internal_error(std::string msg, std::string code = "INTERNAL") {
  return ApiError(500, std::move(code), std::move(msg));
}

}  // namespace fsrv
