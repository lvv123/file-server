#include "cli.h"

#include <charconv>
#include <string_view>
#include <utility>

namespace fsrv {

namespace {

template <typename T>
bool parse_number(std::string_view text, T& out) {
  if (text.empty()) return false;
  const char* begin = text.data();
  const char* end = begin + text.size();
  const auto result = std::from_chars(begin, end, out);
  return result.ec == std::errc() && result.ptr == end;
}

template <typename T>
bool set_once(std::optional<T>& slot, T value, std::string_view name,
              std::string& error) {
  if (slot.has_value()) {
    error = "option '" + std::string(name) + "' was specified more than once";
    return false;
  }
  slot = std::move(value);
  return true;
}

bool take_value(int& index, int argc, char** argv, std::string_view name,
                bool has_inline, const std::string& inline_value,
                std::string& out, std::string& error) {
  if (has_inline) {
    if (inline_value.empty()) {
      error = "option '" + std::string(name) + "' requires a value";
      return false;
    }
    out = inline_value;
    return true;
  }
  if (index + 1 >= argc) {
    error = "option '" + std::string(name) + "' requires a value";
    return false;
  }
  const std::string next = argv[index + 1];
  if (!next.empty() && next.front() == '-') {
    error = "option '" + std::string(name) + "' requires a value";
    return false;
  }
  out = next;
  ++index;
  return true;
}

bool parse_port(const std::string& text, CliOptions& options,
                std::string& error) {
  int port = 0;
  if (!parse_number(text, port)) {
    error = "invalid port: '" + text + "'";
    return false;
  }
  return set_once(options.port, port, "port", error);
}

bool set_string_option(const std::string& name, const std::string& value,
                       CliOptions& options, std::string& error) {
  if (name == "-d" || name == "--dir" || name == "--directory") {
    return set_once(options.dir, value, name, error);
  }
  if (name == "-c" || name == "--config") {
    return set_once(options.config, value, name, error);
  }
  if (name == "-b" || name == "--bind" || name == "--bind-address") {
    return set_once(options.bind_address, value, name, error);
  }
  return true;
}

bool set_numeric_option(const std::string& name, const std::string& value,
                        CliOptions& options, std::string& error) {
  if (name == "--chunk-size-mb") {
    int v = 0;
    if (!parse_number(value, v)) {
      error = "invalid value for " + name + ": '" + value + "'";
      return false;
    }
    return set_once(options.chunk_size_mb, v, name, error);
  }
  if (name == "--session-ttl-hours") {
    int v = 0;
    if (!parse_number(value, v)) {
      error = "invalid value for " + name + ": '" + value + "'";
      return false;
    }
    return set_once(options.session_ttl_hours, v, name, error);
  }
  if (name == "--audit-retention-days") {
    int v = 0;
    if (!parse_number(value, v)) {
      error = "invalid value for " + name + ": '" + value + "'";
      return false;
    }
    return set_once(options.audit_retention_days, v, name, error);
  }
  if (name == "--audit-max-size-mb") {
    int v = 0;
    if (!parse_number(value, v)) {
      error = "invalid value for " + name + ": '" + value + "'";
      return false;
    }
    return set_once(options.audit_max_size_mb, v, name, error);
  }
  if (name == "--thread-min") {
    int v = 0;
    if (!parse_number(value, v)) {
      error = "invalid value for " + name + ": '" + value + "'";
      return false;
    }
    return set_once(options.thread_min, v, name, error);
  }
  if (name == "--thread-max") {
    int v = 0;
    if (!parse_number(value, v)) {
      error = "invalid value for " + name + ": '" + value + "'";
      return false;
    }
    return set_once(options.thread_max, v, name, error);
  }
  if (name == "--thread-max-queue") {
    int v = 0;
    if (!parse_number(value, v)) {
      error = "invalid value for " + name + ": '" + value + "'";
      return false;
    }
    return set_once(options.thread_max_queue, v, name, error);
  }
  if (name == "--io-timeout-sec") {
    int v = 0;
    if (!parse_number(value, v)) {
      error = "invalid value for " + name + ": '" + value + "'";
      return false;
    }
    return set_once(options.io_timeout_sec, v, name, error);
  }
  if (name == "--max-json-body-bytes") {
    int64_t v = 0;
    if (!parse_number(value, v)) {
      error = "invalid value for " + name + ": '" + value + "'";
      return false;
    }
    return set_once(options.max_json_body_bytes, v, name, error);
  }
  if (name == "--search-limit-default") {
    int v = 0;
    if (!parse_number(value, v)) {
      error = "invalid value for " + name + ": '" + value + "'";
      return false;
    }
    return set_once(options.search_limit_default, v, name, error);
  }
  if (name == "--search-limit-max") {
    int v = 0;
    if (!parse_number(value, v)) {
      error = "invalid value for " + name + ": '" + value + "'";
      return false;
    }
    return set_once(options.search_limit_max, v, name, error);
  }
  return true;
}

bool is_numeric_option(std::string_view name) {
  return name == "--chunk-size-mb" || name == "--session-ttl-hours" ||
         name == "--audit-retention-days" || name == "--audit-max-size-mb" ||
         name == "--thread-min" || name == "--thread-max" ||
         name == "--thread-max-queue" || name == "--io-timeout-sec" ||
         name == "--max-json-body-bytes" || name == "--search-limit-default" ||
         name == "--search-limit-max";
}

}  // namespace

CliParseResult parse_cli(int argc, char** argv) {
  CliParseResult result;
  CliOptions& options = result.options;

  auto fail = [&](std::string message) {
    result.error = std::move(message);
    return result;
  };

  for (int i = 1; i < argc; ++i) {
    const std::string arg = argv[i];
    if (arg == "--") {
      for (++i; i < argc; ++i) {
        if (!parse_port(argv[i], options, result.error)) return result;
      }
      break;
    }

    if (arg.empty() || arg.front() != '-') {
      if (!parse_port(arg, options, result.error)) return result;
      continue;
    }

    std::string name = arg;
    std::string inline_value;
    bool has_inline = false;
    if (arg.rfind("--", 0) == 0) {
      const size_t eq = arg.find('=');
      if (eq != std::string::npos) {
        name = arg.substr(0, eq);
        inline_value = arg.substr(eq + 1);
        has_inline = true;
      }
    }

    if (name == "-h" || name == "--help") {
      if (has_inline) return fail("option '" + name + "' does not take a value");
      options.help = true;
      continue;
    }
    if (name == "--version") {
      if (has_inline) return fail("option '" + name + "' does not take a value");
      options.version = true;
      continue;
    }
    if (name == "--check-config") {
      if (has_inline) return fail("option '" + name + "' does not take a value");
      options.check_config = true;
      continue;
    }

    if (name == "-p" || name == "--port") {
      std::string value;
      if (!take_value(i, argc, argv, name, has_inline, inline_value, value,
                      result.error)) {
        return result;
      }
      if (!parse_port(value, options, result.error)) return result;
      continue;
    }

    if (name == "-d" || name == "--dir" || name == "--directory" ||
        name == "-c" || name == "--config" || name == "-b" ||
        name == "--bind" || name == "--bind-address") {
      std::string value;
      if (!take_value(i, argc, argv, name, has_inline, inline_value, value,
                      result.error)) {
        return result;
      }
      if (!set_string_option(name, value, options, result.error)) return result;
      continue;
    }

    if (name == "--admin-allow-ip") {
      std::string value;
      if (!take_value(i, argc, argv, name, has_inline, inline_value, value,
                      result.error)) {
        return result;
      }
      if (!options.admin_allow_ips.has_value()) {
        options.admin_allow_ips.emplace();
      }
      options.admin_allow_ips->push_back(value);
      continue;
    }

    if (is_numeric_option(name)) {
      std::string value;
      if (!take_value(i, argc, argv, name, has_inline, inline_value, value,
                      result.error)) {
        return result;
      }
      if (!set_numeric_option(name, value, options, result.error)) {
        return result;
      }
      continue;
    }

    return fail("unknown option '" + arg + "'");
  }

  return result;
}

std::string cli_usage() {
  return
      "file-server - minimalist LAN file server\n"
      "\n"
      "Usage: file-server [PORT] [options]\n"
      "\n"
      "The served directory is the current working directory by default.\n"
      "\n"
      "Options:\n"
      "  -d, --dir, --directory <path>  Directory to serve (default: cwd)\n"
      "  -p, --port <port>             Listen port (default: 8080)\n"
      "  -b, --bind, --bind-address <addr>\n"
      "                                Listen address (default: 0.0.0.0)\n"
      "  -c, --config <path>           Config file (default: "
      "<dir>/.file-server/config.json)\n"
      "      --chunk-size-mb <n>       Upload chunk size (default: 8)\n"
      "      --session-ttl-hours <n>   Upload session lifetime (default: 24)\n"
      "      --audit-retention-days <n>\n"
      "      --audit-max-size-mb <n>\n"
      "      --admin-allow-ip <ip>     Repeatable; replaces the configured list\n"
      "      --thread-min <n>\n"
      "      --thread-max <n>\n"
      "      --thread-max-queue <n>\n"
      "      --io-timeout-sec <n>\n"
      "      --max-json-body-bytes <n>\n"
      "      --search-limit-default <n>\n"
      "      --search-limit-max <n>\n"
      "      --check-config            Validate and print the effective paths\n"
      "  -h, --help                    Show this help\n"
      "      --version                 Show the version\n"
      "\n"
      "Examples:\n"
      "  file-server\n"
      "  file-server 8007\n"
      "  file-server -d D:\\share 8007\n";
}

void apply_cli_overrides(Config& cfg, const CliOptions& options) {
  if (options.port) cfg.port = *options.port;
  if (options.bind_address) cfg.bind_address = *options.bind_address;
  if (options.chunk_size_mb) cfg.chunk_size_mb = *options.chunk_size_mb;
  if (options.session_ttl_hours) cfg.session_ttl_hours = *options.session_ttl_hours;
  if (options.audit_retention_days) {
    cfg.audit_retention_days = *options.audit_retention_days;
  }
  if (options.audit_max_size_mb) {
    cfg.audit_max_size_mb = *options.audit_max_size_mb;
  }
  if (options.admin_allow_ips) cfg.admin_allow_ips = *options.admin_allow_ips;
  if (options.thread_min) cfg.thread_min = *options.thread_min;
  if (options.thread_max) cfg.thread_max = *options.thread_max;
  if (options.thread_max_queue) cfg.thread_max_queue = *options.thread_max_queue;
  if (options.io_timeout_sec) cfg.io_timeout_sec = *options.io_timeout_sec;
  if (options.max_json_body_bytes) {
    cfg.max_json_body_bytes = *options.max_json_body_bytes;
  }
  if (options.search_limit_default) {
    cfg.search_limit_default = *options.search_limit_default;
  }
  if (options.search_limit_max) cfg.search_limit_max = *options.search_limit_max;
}

}  // namespace fsrv
