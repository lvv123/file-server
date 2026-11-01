#include "config.h"

#include <fstream>
#include <sstream>

#include "ext.h"
#include "util.h"

namespace fsrv {

namespace {

std::string read_file(const fs::path& path) {
  std::ifstream in(path, std::ios::binary);
  if (!in) throw ConfigError("cannot open configuration file: " + path.u8string());
  std::ostringstream ss;
  ss << in.rdbuf();
  return ss.str();
}

bool is_ipv4_literal(const std::string& s) {
  int parts = 0;
  size_t i = 0;
  while (i <= s.size()) {
    size_t j = s.find('.', i);
    if (j == std::string::npos) j = s.size();
    if (j == i || j - i > 3) return false;
    int value = 0;
    for (size_t k = i; k < j; ++k) {
      if (s[k] < '0' || s[k] > '9') return false;
      value = value * 10 + (s[k] - '0');
    }
    if (value > 255) return false;
    ++parts;
    if (j == s.size()) break;
    i = j + 1;
  }
  return parts == 4;
}

bool is_ipv6_literal(const std::string& s) {
  if (s.find(':') == std::string::npos) return false;
  int colons = 0;
  for (char c : s) {
    const bool ok = (c >= '0' && c <= '9') || (c >= 'a' && c <= 'f') ||
                    (c >= 'A' && c <= 'F') || c == ':' || c == '.';
    if (!ok) return false;
    if (c == ':') ++colons;
  }
  return colons >= 2;
}

std::string require_string(const Json& j, const char* key,
                           const std::string& fallback) {
  if (!j.contains(key)) return fallback;
  if (!j[key].is_string()) {
    throw ConfigError(std::string("config key '") + key + "' must be a string");
  }
  return j[key].get<std::string>();
}

int require_int(const Json& j, const char* key, int fallback) {
  if (!j.contains(key)) return fallback;
  if (!j[key].is_number_integer()) {
    throw ConfigError(std::string("config key '") + key +
                      "' must be an integer");
  }
  return j[key].get<int>();
}

int64_t require_int64(const Json& j, const char* key, int64_t fallback) {
  if (!j.contains(key)) return fallback;
  if (!j[key].is_number_integer()) {
    throw ConfigError(std::string("config key '") + key +
                      "' must be an integer");
  }
  return j[key].get<int64_t>();
}

}  // namespace

Config load_config(const fs::path& path) {
  const std::string text = read_file(path);

  Json j;
  try {
    j = Json::parse(text);
  } catch (const std::exception& e) {
    throw ConfigError(std::string("invalid JSON in ") + path.u8string() + ": " +
                      e.what());
  }
  if (!j.is_object()) throw ConfigError("configuration root must be an object");

  Config cfg;
  cfg.port = require_int(j, "port", cfg.port);
  cfg.bind_address = require_string(j, "bind_address", cfg.bind_address);
  cfg.data_root = require_string(j, "data_root", cfg.data_root);
  cfg.chunk_size_mb = require_int(j, "chunk_size_mb", cfg.chunk_size_mb);
  cfg.session_ttl_hours = require_int(j, "session_ttl_hours", cfg.session_ttl_hours);
  cfg.audit_retention_days =
      require_int(j, "audit_retention_days", cfg.audit_retention_days);
  cfg.audit_max_size_mb =
      require_int(j, "audit_max_size_mb", cfg.audit_max_size_mb);

  if (j.contains("admin_allow_ips")) {
    if (!j["admin_allow_ips"].is_array()) {
      throw ConfigError("config key 'admin_allow_ips' must be an array");
    }
    cfg.admin_allow_ips.clear();
    for (const auto& item : j["admin_allow_ips"]) {
      if (!item.is_string()) {
        throw ConfigError("every entry of 'admin_allow_ips' must be a string");
      }
      cfg.admin_allow_ips.push_back(item.get<std::string>());
    }
    if (cfg.admin_allow_ips.empty()) {
      cfg.admin_allow_ips.push_back("127.0.0.1");
    }
  }

  cfg.thread_min = require_int(j, "thread_min", cfg.thread_min);
  cfg.thread_max = require_int(j, "thread_max", cfg.thread_max);
  cfg.thread_max_queue = require_int(j, "thread_max_queue", cfg.thread_max_queue);
  cfg.io_timeout_sec = require_int(j, "io_timeout_sec", cfg.io_timeout_sec);
  cfg.max_json_body_bytes =
      require_int64(j, "max_json_body_bytes", cfg.max_json_body_bytes);
  cfg.search_limit_default =
      require_int(j, "search_limit_default", cfg.search_limit_default);
  cfg.search_limit_max = require_int(j, "search_limit_max", cfg.search_limit_max);

  const std::string err = validate_config(cfg);
  if (!err.empty()) throw ConfigError(err);
  return cfg;
}

std::string validate_config(const Config& cfg) {
  if (cfg.port < 1 || cfg.port > 65535) return "port must be in 1..65535";
  if (cfg.data_root.empty()) return "data_root must not be empty";
  if (cfg.chunk_size_mb < 1 || cfg.chunk_size_mb > 1024) {
    return "chunk_size_mb must be in 1..1024";
  }
  if (cfg.session_ttl_hours < 1) return "session_ttl_hours must be >= 1";
  if (cfg.audit_retention_days < 0) return "audit_retention_days must be >= 0";
  if (cfg.audit_max_size_mb < 0) return "audit_max_size_mb must be >= 0";
  if (cfg.thread_min < 1) return "thread_min must be >= 1";
  if (cfg.thread_max < cfg.thread_min) {
    return "thread_max must be >= thread_min";
  }
  if (cfg.thread_max_queue < 1) return "thread_max_queue must be >= 1";
  if (cfg.io_timeout_sec < 1) return "io_timeout_sec must be >= 1";
  if (cfg.max_json_body_bytes < 1024) {
    return "max_json_body_bytes must be >= 1024";
  }
  if (cfg.search_limit_default < 1 || cfg.search_limit_max < 1) {
    return "search limits must be >= 1";
  }
  if (cfg.search_limit_default > cfg.search_limit_max) {
    return "search_limit_default must be <= search_limit_max";
  }
  if (cfg.bind_address.empty()) return "bind_address must not be empty";
  for (const auto& ip : cfg.admin_allow_ips) {
    if (!is_ipv4_literal(ip) && !is_ipv6_literal(ip)) {
      return "admin_allow_ips contains an invalid IP address: " + ip;
    }
  }
  return {};
}

fs::path resolve_data_root(const Config& cfg) {
  fs::path p = fs::u8path(cfg.data_root);
  if (p.is_relative()) p = fs::absolute(p);
  return p.lexically_normal();
}

}  // namespace fsrv
