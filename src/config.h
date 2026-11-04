#pragma once

#include <cstdint>
#include <string>
#include <vector>

#include "compat.h"

namespace fsrv {

struct Config {
  int port = 8080;
  std::string bind_address = "0.0.0.0";

  int chunk_size_mb = 8;
  int session_ttl_hours = 24;
  int audit_retention_days = 30;
  int audit_max_size_mb = 512;

  std::vector<std::string> admin_allow_ips{"127.0.0.1"};

  // Optional tuning knobs (defaults follow the specification).
  int thread_min = 16;
  int thread_max = 32;
  int thread_max_queue = 64;

  int io_timeout_sec = 300;
  int64_t max_json_body_bytes = 1 << 20;  // 1 MiB
  int search_limit_default = 200;
  int search_limit_max = 1000;

  int64_t chunk_size_bytes() const {
    return static_cast<int64_t>(chunk_size_mb) * 1024 * 1024;
  }
  int64_t session_ttl_seconds() const {
    return static_cast<int64_t>(session_ttl_hours) * 3600;
  }
};

// Thrown when the configuration file exists but is invalid.
struct ConfigError : std::runtime_error {
  using std::runtime_error::runtime_error;
};

// Loads the JSON file at `path`. A missing file is an error; missing keys fall
// back to the defaults above. Throws ConfigError on invalid content.
Config load_config(const fs::path& path);

// Like load_config(), but returns the built-in defaults when the file does not
// exist. Invalid content is still an error.
Config load_config_or_default(const fs::path& path);

// Writes a documented default configuration. Used only when the default
// location (<serve-root>/.file-server/config.json) does not exist yet.
void write_default_config(const fs::path& path);

// Validates a config and returns a human readable error message, or an empty
// string when the config is acceptable.
std::string validate_config(const Config& cfg);

}  // namespace fsrv
