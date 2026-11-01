#include <cstdint>

#include "error.h"
#include "handler_routes.h"
#include "upload.h"
#include "util.h"

namespace fsrv {

namespace {

bool parse_uint32(const std::string& s, uint32_t& out) {
  if (s.empty() || s.size() > 10) return false;
  uint64_t value = 0;
  for (char c : s) {
    if (c < '0' || c > '9') return false;
    value = value * 10 + static_cast<uint64_t>(c - '0');
  }
  if (value > 0xFFFFFFFFULL) return false;
  out = static_cast<uint32_t>(value);
  return true;
}

Json session_json(const UploadSessionInfo& info) {
  Json data;
  data["session_id"] = info.id;
  data["path"] = "/" + info.rel_path;
  data["total_size"] = info.total_size;
  data["total_chunks"] = info.total_chunks;
  data["chunk_size"] = info.chunk_size;
  data["created_at"] = info.created_at;
  data["expires_at"] = info.expires_at;
  data["received"] = info.received;
  data["received_count"] = info.received.size();
  return data;
}

}  // namespace

void handle_upload_init(const AppContext& ctx, const httplib::Request& req,
                        httplib::Response& res) {
  handle(req, res, "upload_init", [&](RequestScope& scope) {
    const Json body = parse_json_body(req);
    const std::string user_path = require_string_field(body, "path");
    const int64_t total_size = require_int_field(body, "total_size");
    scope.set_path(user_path);
    scope.set_size(total_size);

    const UploadSessionInfo info = upload_init(ctx, user_path, total_size);
    scope.ok(session_json(info));
  });
}

void handle_upload_chunk(const AppContext& ctx, const httplib::Request& req,
                         httplib::Response& res,
                         const httplib::ContentReader& reader) {
  handle(req, res, "upload_chunk", [&](RequestScope& scope) {
    scope.audit().skip_success();
    BodyGate gate(reader);

    try {
      const std::string session_id =
          req.has_header("X-Session-Id") ? req.get_header_value("X-Session-Id")
                                         : std::string();
      const std::string index_text =
          req.has_header("X-Chunk-Index") ? req.get_header_value("X-Chunk-Index")
                                          : std::string();
      uint32_t index = 0;
      if (session_id.empty() || index_text.empty() ||
          !parse_uint32(index_text, index)) {
        throw bad_request(
            "X-Session-Id and a numeric X-Chunk-Index header are required",
            "MISSING_CHUNK_HEADERS");
      }

      const bool has_length = req.has_header("Content-Length");
      int64_t content_length = -1;
      if (has_length) {
        try {
          content_length = std::stoll(req.get_header_value("Content-Length"));
        } catch (...) {
          content_length = -1;
        }
      }

      const ChunkResult chunk =
          upload_chunk(ctx, session_id, index, gate, has_length, content_length);

      Json data;
      data["session_id"] = session_id;
      data["index"] = index;
      data["received"] = chunk.received;
      data["total_chunks"] = chunk.total_chunks;
      data["complete"] = chunk.complete;
      scope.ok(std::move(data));
    } catch (...) {
      // The request arrived through the content-reader route, so the body may
      // still be in flight. Drain whatever is left before the error response
      // goes out, otherwise the leftover bytes desynchronise the keep-alive
      // connection and corrupt the next request on it. BodyGate guarantees this
      // is a no-op when the body has already been read.
      gate.drain();
      throw;
    }
  });
}

void handle_upload_session(const AppContext& ctx, const httplib::Request& req,
                           httplib::Response& res) {
  handle(req, res, "upload_status", [&](RequestScope& scope) {
    const std::string id = req.get_param_value("id");
    if (id.empty()) {
      throw bad_request("query parameter 'id' is required", "MISSING_SESSION_ID");
    }
    const UploadSessionInfo info = upload_received(ctx, id);
    scope.set_path(info.rel_path);
    scope.ok(session_json(info));
  });
}

void handle_upload_complete(const AppContext& ctx, const httplib::Request& req,
                            httplib::Response& res) {
  handle(req, res, "upload", [&](RequestScope& scope) {
    const Json body = parse_json_body(req);
    const std::string session_id = require_string_field(body, "session_id");

    // Read the session first so the audit row carries the target path.
    const UploadSessionInfo info = upload_received(ctx, session_id);
    scope.set_path(info.rel_path);
    scope.set_size(info.total_size);

    upload_complete(ctx, session_id);

    Json data;
    data["path"] = "/" + info.rel_path;
    data["size"] = info.total_size;
    scope.ok(std::move(data));
  });
}

}  // namespace fsrv
