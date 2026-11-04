#pragma once

#include <cstddef>
#include <cstdint>
#include <functional>
#include <string>
#include <string_view>
#include <vector>

#include "app_context.h"

namespace fsrv {

// Framework independent view of a request body. The transport layer implements
// it (see BodyGate) so the upload logic can be unit tested without constructing
// a cpp-httplib ContentReader.
struct BodySource {
  // Streams the body into `sink`. Returns false when the body could not be
  // read. Only one call per request is meaningful.
  virtual bool read(const std::function<bool(const char*, size_t)>& sink) = 0;

  // Discards whatever part of the body has not been read yet.
  virtual void drain() = 0;

  // Marks the body as handled without reading it (used when there is no
  // Content-Length, where draining would block until the peer closes).
  virtual void abandon() = 0;

  virtual ~BodySource() = default;
};

struct UploadSessionInfo {
  std::string id;
  std::string rel_path;
  int64_t total_size = 0;
  uint32_t total_chunks = 0;
  uint32_t chunk_size = 0;
  int64_t created_at = 0;
  int64_t expires_at = 0;
  std::vector<uint32_t> received;
};

UploadSessionInfo upload_init(const AppContext& ctx, std::string_view user_path,
                              int64_t total_size);

UploadSessionInfo upload_received(const AppContext& ctx,
                                  std::string_view session_id);

struct ChunkResult {
  uint32_t received = 0;
  uint32_t total_chunks = 0;
  bool complete = false;
};

// Streams a raw chunk body to tmp/<session>/<index>.part. The body is always
// consumed so that keep-alive connections stay aligned even on errors; the
// caller passes a BodySource (BodyGate on the transport layer) so the body is
// read at most once per request.
ChunkResult upload_chunk(const AppContext& ctx, std::string_view session_id,
                         uint32_t index, BodySource& body,
                         bool has_content_length, int64_t content_length);

// Concatenates the chunks, verifies the byte count and atomically moves the
// result into place.
void upload_complete(const AppContext& ctx, std::string_view session_id);

// Removes expired sessions (DB row + chunk directory). Returns the count.
int64_t upload_cleanup_expired(const AppContext& ctx);

// Removes chunk directories that have no matching live session (start-up
// recovery). Returns the count.
int64_t upload_cleanup_orphans(const AppContext& ctx);

}  // namespace fsrv
