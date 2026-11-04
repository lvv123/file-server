#include <string>
#include <vector>

#include "cli.h"
#include "test_helpers.h"

using fsrv::apply_cli_overrides;
using fsrv::CliOptions;
using fsrv::CliParseResult;
using fsrv::Config;
using fsrv::parse_cli;

namespace {

CliParseResult parse(std::vector<std::string> args) {
  std::vector<char*> argv;
  argv.reserve(args.size() + 1);
  argv.push_back(const_cast<char*>("file-server"));
  for (std::string& arg : args) argv.push_back(arg.data());
  return parse_cli(static_cast<int>(argv.size()), argv.data());
}

}  // namespace

TEST_CASE(cli_no_arguments_uses_all_defaults) {
  const CliParseResult r = parse({});
  CHECK_EQ(r.error, std::string(""));
  CHECK(!r.options.port.has_value());
  CHECK(!r.options.dir.has_value());
  CHECK(!r.options.config.has_value());
  CHECK(!r.options.help);
  CHECK(!r.options.version);
  CHECK(!r.options.check_config);
}

TEST_CASE(cli_positional_port_matches_the_python_style) {
  const CliParseResult r = parse({"8007"});
  CHECK_EQ(r.error, std::string(""));
  REQUIRE(r.options.port.has_value());
  CHECK_EQ(*r.options.port, 8007);
}

TEST_CASE(cli_long_options_accept_both_spellings) {
  const CliParseResult r = parse(
      {"--dir=D:\\share", "--port=9000", "--bind-address=127.0.0.1",
       "--chunk-size-mb", "4", "--session-ttl-hours=48",
       "--audit-retention-days", "7", "--audit-max-size-mb=128",
       "--thread-min", "2", "--thread-max=4", "--thread-max-queue", "8",
       "--io-timeout-sec=30", "--max-json-body-bytes", "4096",
       "--search-limit-default=20", "--search-limit-max", "50"});
  CHECK_EQ(r.error, std::string(""));
  CHECK_EQ(*r.options.dir, std::string("D:\\share"));
  CHECK_EQ(*r.options.port, 9000);
  CHECK_EQ(*r.options.bind_address, std::string("127.0.0.1"));
  CHECK_EQ(*r.options.chunk_size_mb, 4);
  CHECK_EQ(*r.options.session_ttl_hours, 48);
  CHECK_EQ(*r.options.audit_retention_days, 7);
  CHECK_EQ(*r.options.audit_max_size_mb, 128);
  CHECK_EQ(*r.options.thread_min, 2);
  CHECK_EQ(*r.options.thread_max, 4);
  CHECK_EQ(*r.options.thread_max_queue, 8);
  CHECK_EQ(*r.options.io_timeout_sec, 30);
  CHECK_EQ(*r.options.max_json_body_bytes, static_cast<int64_t>(4096));
  CHECK_EQ(*r.options.search_limit_default, 20);
  CHECK_EQ(*r.options.search_limit_max, 50);
}

TEST_CASE(cli_short_options_and_positional_port_can_be_combined) {
  const CliParseResult r = parse({"-d", "C:\\share", "-p", "8007", "-b",
                                  "0.0.0.0", "-c", "my.json"});
  CHECK_EQ(r.error, std::string(""));
  CHECK_EQ(*r.options.dir, std::string("C:\\share"));
  CHECK_EQ(*r.options.port, 8007);
  CHECK_EQ(*r.options.bind_address, std::string("0.0.0.0"));
  CHECK_EQ(*r.options.config, std::string("my.json"));
}

TEST_CASE(cli_admin_allow_ip_is_repeatable) {
  const CliParseResult r =
      parse({"--admin-allow-ip", "127.0.0.1", "--admin-allow-ip=::1"});
  CHECK_EQ(r.error, std::string(""));
  REQUIRE(r.options.admin_allow_ips.has_value());
  REQUIRE(r.options.admin_allow_ips->size() == static_cast<size_t>(2));
  CHECK_EQ((*r.options.admin_allow_ips)[0], std::string("127.0.0.1"));
  CHECK_EQ((*r.options.admin_allow_ips)[1], std::string("::1"));
}

TEST_CASE(cli_flags_are_recognised) {
  const CliParseResult r = parse({"--check-config", "--help", "--version"});
  CHECK_EQ(r.error, std::string(""));
  CHECK(r.options.check_config);
  CHECK(r.options.help);
  CHECK(r.options.version);
}

TEST_CASE(cli_duplicate_or_missing_values_are_errors) {
  CHECK(!parse({"--port", "8007", "8008"}).error.empty());
  CHECK(!parse({"--port", "8007", "--port", "8008"}).error.empty());
  CHECK(!parse({"--dir"}).error.empty());
  CHECK(!parse({"--dir", "--port", "8007"}).error.empty());
  CHECK(!parse({"--port", "not-a-number"}).error.empty());
  CHECK(!parse({"--no-such-option"}).error.empty());
}

TEST_CASE(cli_apply_overrides_only_touches_explicit_values) {
  Config cfg;
  CliOptions options;
  options.port = 9000;
  options.chunk_size_mb = 4;
  options.admin_allow_ips = std::vector<std::string>{"10.0.0.1"};

  apply_cli_overrides(cfg, options);
  CHECK_EQ(cfg.port, 9000);
  CHECK_EQ(cfg.chunk_size_mb, 4);
  REQUIRE(cfg.admin_allow_ips.size() == static_cast<size_t>(1));
  CHECK_EQ(cfg.admin_allow_ips[0], std::string("10.0.0.1"));
  CHECK_EQ(cfg.session_ttl_hours, 24);  // untouched default
}
