#include <string>

#include "config.h"
#include "test_helpers.h"

namespace fs = std::filesystem;

using fsrv::Config;
using fsrv::ConfigError;
using fsrv::load_config;
using fsrv::load_config_or_default;
using fsrv::validate_config;
using fsrv::write_default_config;

namespace {

bool load_fails(const fs::path& path) {
  try {
    (void)load_config(path);
  } catch (const ConfigError&) {
    return true;
  } catch (...) {
    return false;
  }
  return false;
}

}  // namespace

TEST_CASE(config_missing_keys_use_defaults) {
  testutil::TempDir dir{"fsrv-config"};
  testutil::write_bytes(dir / "c.json", "{}");
  const Config cfg = load_config(dir / "c.json");
  CHECK_EQ(cfg.port, 8080);
  CHECK_EQ(cfg.bind_address, std::string("0.0.0.0"));
  CHECK_EQ(cfg.chunk_size_mb, 8);
  CHECK_EQ(cfg.session_ttl_hours, 24);
  CHECK_EQ(cfg.audit_retention_days, 30);
  CHECK_EQ(cfg.audit_max_size_mb, 512);
  REQUIRE(cfg.admin_allow_ips.size() == static_cast<size_t>(1));
  CHECK_EQ(cfg.admin_allow_ips[0], std::string("127.0.0.1"));
  CHECK_EQ(cfg.thread_min, 16);
  CHECK_EQ(cfg.thread_max, 32);
  CHECK_EQ(cfg.thread_max_queue, 64);
  CHECK_EQ(cfg.search_limit_default, 200);
  CHECK_EQ(cfg.search_limit_max, 1000);
}

TEST_CASE(config_partial_override) {
  testutil::TempDir dir{"fsrv-config"};
  testutil::write_bytes(dir / "c.json",
                        R"({"port": 9000, "chunk_size_mb": 4,
                            "session_ttl_hours": 2})");
  const Config cfg = load_config(dir / "c.json");
  CHECK_EQ(cfg.port, 9000);
  CHECK_EQ(cfg.chunk_size_mb, 4);
  CHECK_EQ(cfg.session_ttl_hours, 2);
  CHECK_EQ(cfg.bind_address, std::string("0.0.0.0"));
  CHECK_EQ(cfg.chunk_size_bytes(), static_cast<int64_t>(4 * 1024 * 1024));
  CHECK_EQ(cfg.session_ttl_seconds(), static_cast<int64_t>(7200));
}

TEST_CASE(config_full_document) {
  testutil::TempDir dir{"fsrv-config"};
  testutil::write_bytes(dir / "c.json", R"({
    "port": 8081,
    "bind_address": "127.0.0.1",
    "chunk_size_mb": 16,
    "session_ttl_hours": 48,
    "audit_retention_days": 7,
    "audit_max_size_mb": 128,
    "admin_allow_ips": ["127.0.0.1", "::1", "192.168.1.10"],
    "thread_min": 4,
    "thread_max": 8,
    "thread_max_queue": 16,
    "io_timeout_sec": 60,
    "max_json_body_bytes": 4096,
    "search_limit_default": 50,
    "search_limit_max": 100
  })");
  const Config cfg = load_config(dir / "c.json");
  CHECK_EQ(cfg.port, 8081);
  CHECK_EQ(cfg.bind_address, std::string("127.0.0.1"));
  CHECK_EQ(cfg.chunk_size_mb, 16);
  CHECK_EQ(cfg.session_ttl_hours, 48);
  CHECK_EQ(cfg.audit_retention_days, 7);
  CHECK_EQ(cfg.audit_max_size_mb, 128);
  REQUIRE(cfg.admin_allow_ips.size() == static_cast<size_t>(3));
  CHECK_EQ(cfg.admin_allow_ips[1], std::string("::1"));
  CHECK_EQ(cfg.thread_min, 4);
  CHECK_EQ(cfg.thread_max, 8);
  CHECK_EQ(cfg.thread_max_queue, 16);
  CHECK_EQ(cfg.io_timeout_sec, 60);
  CHECK_EQ(cfg.max_json_body_bytes, static_cast<int64_t>(4096));
  CHECK_EQ(cfg.search_limit_default, 50);
  CHECK_EQ(cfg.search_limit_max, 100);
}

TEST_CASE(config_empty_admin_list_falls_back_to_loopback) {
  testutil::TempDir dir{"fsrv-config"};
  testutil::write_bytes(dir / "c.json", R"({"admin_allow_ips": []})");
  const Config cfg = load_config(dir / "c.json");
  REQUIRE(cfg.admin_allow_ips.size() == static_cast<size_t>(1));
  CHECK_EQ(cfg.admin_allow_ips[0], std::string("127.0.0.1"));
}

TEST_CASE(config_missing_file_is_an_error) {
  testutil::TempDir dir{"fsrv-config"};
  CHECK(load_fails(dir / "does-not-exist.json"));
}

TEST_CASE(config_load_or_default_handles_a_missing_file) {
  testutil::TempDir dir{"fsrv-config"};
  const Config cfg = load_config_or_default(dir / "does-not-exist.json");
  CHECK_EQ(cfg.port, 8080);
  CHECK_EQ(cfg.chunk_size_mb, 8);
}

TEST_CASE(config_data_root_is_rejected) {
  testutil::TempDir dir{"fsrv-config"};
  testutil::write_bytes(dir / "c.json", R"({"data_root": "./data"})");
  CHECK(load_fails(dir / "c.json"));
}

TEST_CASE(config_malformed_json_is_an_error) {
  testutil::TempDir dir{"fsrv-config"};
  testutil::write_bytes(dir / "c.json", "{ this is not json ");
  CHECK(load_fails(dir / "c.json"));
  testutil::write_bytes(dir / "c2.json", "[]");
  CHECK(load_fails(dir / "c2.json"));
  testutil::write_bytes(dir / "c3.json", R"({"port": "8080"})");
  CHECK(load_fails(dir / "c3.json"));
  testutil::write_bytes(dir / "c4.json", R"({"admin_allow_ips": "127.0.0.1"})");
  CHECK(load_fails(dir / "c4.json"));
  testutil::write_bytes(dir / "c5.json", R"({"admin_allow_ips": [1, 2]})");
  CHECK(load_fails(dir / "c5.json"));
}

TEST_CASE(config_out_of_range_values_are_rejected) {
  testutil::TempDir dir{"fsrv-config"};
  testutil::write_bytes(dir / "port0.json", R"({"port": 0})");
  CHECK(load_fails(dir / "port0.json"));
  testutil::write_bytes(dir / "portx.json", R"({"port": 70000})");
  CHECK(load_fails(dir / "portx.json"));
  testutil::write_bytes(dir / "chunk0.json", R"({"chunk_size_mb": 0})");
  CHECK(load_fails(dir / "chunk0.json"));
  testutil::write_bytes(dir / "chunkx.json", R"({"chunk_size_mb": 5000})");
  CHECK(load_fails(dir / "chunkx.json"));
  testutil::write_bytes(dir / "ttl.json", R"({"session_ttl_hours": 0})");
  CHECK(load_fails(dir / "ttl.json"));
  testutil::write_bytes(dir / "threads.json",
                        R"({"thread_min": 8, "thread_max": 4})");
  CHECK(load_fails(dir / "threads.json"));
  testutil::write_bytes(dir / "search.json",
                        R"({"search_limit_default": 500,
                            "search_limit_max": 100})");
  CHECK(load_fails(dir / "search.json"));
  testutil::write_bytes(dir / "ip.json",
                        R"({"admin_allow_ips": ["localhost"]})");
  CHECK(load_fails(dir / "ip.json"));
  testutil::write_bytes(dir / "ip2.json",
                        R"({"admin_allow_ips": ["10.0.0.256"]})");
  CHECK(load_fails(dir / "ip2.json"));
  testutil::write_bytes(dir / "bind.json", R"({"bind_address": ""})");
  CHECK(load_fails(dir / "bind.json"));
}

TEST_CASE(config_validate_reports_clear_messages) {
  Config cfg;
  CHECK_EQ(validate_config(cfg), std::string(""));

  cfg.port = 0;
  CHECK(!validate_config(cfg).empty());
  cfg.port = 8080;

  cfg.chunk_size_mb = 2048;
  CHECK(!validate_config(cfg).empty());
  cfg.chunk_size_mb = 8;

  cfg.thread_max = 2;
  CHECK(!validate_config(cfg).empty());
  cfg.thread_max = 32;

  cfg.audit_max_size_mb = -1;
  CHECK(!validate_config(cfg).empty());
  cfg.audit_max_size_mb = 0;  // disabling the capacity rule is legal
  CHECK_EQ(validate_config(cfg), std::string(""));

  cfg.admin_allow_ips.push_back("not-an-ip");
  CHECK(!validate_config(cfg).empty());
}

TEST_CASE(config_default_file_round_trips) {
  testutil::TempDir dir{"fsrv-config"};
  const fs::path path = dir / "nested" / "config.json";
  fs::create_directories(path.parent_path());
  write_default_config(path);
  CHECK(fs::exists(path));
  const Config cfg = load_config(path);
  CHECK_EQ(cfg.port, 8080);
  CHECK_EQ(cfg.chunk_size_mb, 8);
  CHECK_EQ(cfg.admin_allow_ips.size(), static_cast<size_t>(1));
}
