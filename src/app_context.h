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
  fs::path data_root;
  fs::path files_root;
  fs::path tmp_root;
  fs::path db_path;
  std::string config_path;
  int64_t started_at = 0;
};

}  // namespace fsrv
