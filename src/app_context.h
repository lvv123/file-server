#pragma once

#include <string>

#include "compat.h"
#include "config.h"
#include "paths.h"

namespace fsrv {

// Everything a request handler needs, built once at start-up and immutable
// afterwards.
struct AppContext {
  Config cfg;
  PathGuard paths;
  fs::path serve_root;  // the directory exposed through the file API
  fs::path meta_root;   // <serve_root>/.file-server
  fs::path tmp_root;    // <meta_root>/tmp
  fs::path db_path;     // <meta_root>/meta.db
  fs::path config_path;
  int64_t started_at = 0;
};

}  // namespace fsrv
