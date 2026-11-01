#pragma once

#include <cstddef>
#include <string_view>

namespace fsrv {

struct StaticAsset {
  const char* path;
  const char* content_type;
  const char* data;
  size_t size;
};

// Returns the embedded asset for an exact request path ("/", "/app.js", ...),
// or nullptr when the path is not an asset.
const StaticAsset* find_static_asset(std::string_view path);

}  // namespace fsrv
