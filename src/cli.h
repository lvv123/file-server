#pragma once

#include <optional>
#include <string>
#include <vector>

#include "config.h"

namespace fsrv {

// Optional values keep the three-layer precedence explicit:
// built-in defaults < config.json < command line.
struct CliOptions {
  std::optional<int> port;
  std::optional<std::string> dir;
  std::optional<std::string> config;
  std::optional<std::string> bind_address;
  std::optional<int> chunk_size_mb;
  std::optional<int> session_ttl_hours;
  std::optional<int> audit_retention_days;
  std::optional<int> audit_max_size_mb;
  std::optional<std::vector<std::string>> admin_allow_ips;
  std::optional<int> thread_min;
  std::optional<int> thread_max;
  std::optional<int> thread_max_queue;
  std::optional<int> io_timeout_sec;
  std::optional<int64_t> max_json_body_bytes;
  std::optional<int> search_limit_default;
  std::optional<int> search_limit_max;

  bool check_config = false;
  bool help = false;
  bool version = false;
};

struct CliParseResult {
  CliOptions options;
  std::string error;
};

// Parses argv. Supports `--name value`, `--name=value`, and a single positional
// port (the `python -m http.server 8007` form).
CliParseResult parse_cli(int argc, char** argv);

std::string cli_usage();

// Applies every explicitly supplied value to `cfg`.
void apply_cli_overrides(Config& cfg, const CliOptions& options);

}  // namespace fsrv
