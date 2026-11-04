#include <chrono>
#include <csignal>
#include <cstdio>
#include <cstring>
#include <string>
#include <system_error>
#include <thread>

#include "app_context.h"
#include "cli.h"
#include "config.h"
#include "db.h"
#include "error.h"
#include "fsops.h"
#include "instance_lock.h"
#include "layout.h"
#include "paths.h"
#include "repo_audit.h"
#include "repo_files.h"
#include "server.h"
#include "upload.h"
#include "util.h"

namespace fs = fsrv::fs;

namespace {

volatile std::sig_atomic_t g_stop_flag = 0;

void on_signal(int /*signum*/) { g_stop_flag = 1; }

void print_version() { std::printf("file-server 2.0.0\n"); }

void log_banner(const fsrv::AppContext& ctx) {
  fsrv::log_info("serve root  : " + ctx.serve_root.u8string());
  fsrv::log_info("metadata    : " + ctx.meta_root.u8string());
  fsrv::log_info("tmp root    : " + ctx.tmp_root.u8string());
  fsrv::log_info("database    : " + ctx.db_path.u8string());
  fsrv::log_info("config      : " + ctx.config_path.u8string());
  fsrv::log_info("bind        : " + ctx.cfg.bind_address + ":" +
                 std::to_string(ctx.cfg.port));
  fsrv::log_info("chunk size  : " + std::to_string(ctx.cfg.chunk_size_mb) + " MiB");
  fsrv::log_info("session TTL : " + std::to_string(ctx.cfg.session_ttl_hours) + " h");
  std::string admin_ips;
  for (const auto& ip : ctx.cfg.admin_allow_ips) {
    if (!admin_ips.empty()) admin_ips += ", ";
    admin_ips += ip;
  }
  fsrv::log_info("admin IPs   : " + admin_ips);
}

void log_effective_config(const fsrv::Config& cfg) {
  fsrv::log_info("port        : " + std::to_string(cfg.port));
  fsrv::log_info("bind        : " + cfg.bind_address);
  fsrv::log_info("threads     : " + std::to_string(cfg.thread_min) + ".." +
                 std::to_string(cfg.thread_max) + " (queue " +
                 std::to_string(cfg.thread_max_queue) + ")");
  fsrv::log_info("io timeout  : " + std::to_string(cfg.io_timeout_sec) + " s");
  fsrv::log_info("json body   : " + std::to_string(cfg.max_json_body_bytes) +
                 " bytes");
  fsrv::log_info("search      : default " +
                 std::to_string(cfg.search_limit_default) + ", max " +
                 std::to_string(cfg.search_limit_max));
  fsrv::log_info("audit       : " + std::to_string(cfg.audit_retention_days) +
                 " days, " + std::to_string(cfg.audit_max_size_mb) + " MiB");
}

bool looks_like_legacy_layout(const fsrv::RuntimeLayout& layout) {
  std::error_code ec;
  if (fs::exists(layout.meta_root, ec)) return false;
  return fs::exists(layout.serve_root / "files", ec) &&
         fs::exists(layout.serve_root / "tmp", ec) &&
         fs::exists(layout.serve_root / "meta.db", ec);
}

}  // namespace

int main(int argc, char** argv) {
  const fsrv::CliParseResult parsed = fsrv::parse_cli(argc, argv);
  if (!parsed.error.empty()) {
    std::fprintf(stderr, "error: %s\n", parsed.error.c_str());
    std::fputs(fsrv::cli_usage().c_str(), stderr);
    return 2;
  }
  if (parsed.options.help) {
    std::fputs(fsrv::cli_usage().c_str(), stdout);
    return 0;
  }
  if (parsed.options.version) {
    print_version();
    return 0;
  }

  fsrv::AppContext ctx;
  try {
    const fs::path serve_root =
        fsrv::resolve_serve_root(parsed.options.dir.value_or(std::string()));
    const fsrv::RuntimeLayout layout = fsrv::make_layout(serve_root);
    const bool legacy_layout = looks_like_legacy_layout(layout);

    fsrv::validate_meta_dir_if_exists(layout.meta_root);

    const bool explicit_config = parsed.options.config.has_value();
    fs::path config_path =
        explicit_config ? fs::u8path(*parsed.options.config) : layout.config_path;
    if (config_path.is_relative()) config_path = fs::absolute(config_path);
    config_path = config_path.lexically_normal();

    std::error_code ec;
    const bool config_exists = fs::exists(config_path, ec);
    if (ec) {
      throw fsrv::ConfigError("cannot inspect configuration file: " +
                             config_path.u8string() + ": " + ec.message());
    }
    if (!config_exists && explicit_config) {
      throw fsrv::ConfigError("configuration file not found: " +
                             config_path.u8string());
    }

    fsrv::Config cfg;
    if (config_exists) cfg = fsrv::load_config(config_path);
    fsrv::apply_cli_overrides(cfg, parsed.options);

    const std::string config_error = fsrv::validate_config(cfg);
    if (!config_error.empty()) throw fsrv::ConfigError(config_error);

    ctx.cfg = cfg;
    ctx.serve_root = layout.serve_root;
    ctx.meta_root = layout.meta_root;
    ctx.tmp_root = layout.tmp_root;
    ctx.db_path = layout.db_path;
    ctx.config_path = config_path;

    const bool config_is_public =
        config_exists &&
        fsrv::PathGuard::is_within(config_path, layout.serve_root) &&
        !fsrv::PathGuard::is_within(config_path, layout.meta_root);

    if (parsed.options.check_config) {
      log_banner(ctx);
      log_effective_config(ctx.cfg);
      if (config_is_public) {
        fsrv::log_warn("configuration file is inside the served directory and "
                       "can be downloaded: " +
                       config_path.u8string());
      }
      if (!config_exists && !explicit_config) {
        fsrv::log_info("config note : the default file would be created at " +
                       layout.config_path.u8string());
      }
      fsrv::log_info("configuration check passed");
      return 0;
    }

    fsrv::ensure_layout_directories(layout);
    fsrv::InstanceLock instance_lock(layout.meta_root / "server.lock");

    if (!config_exists && !explicit_config) {
      fsrv::write_default_config(layout.config_path);
      fsrv::log_info("created default configuration: " +
                     layout.config_path.u8string());
    }

    if (legacy_layout) {
      fsrv::log_warn(
          "this directory looks like a v1 data root (files/, tmp/, meta.db); "
          "those files are now served as ordinary entries. Use --dir " +
          (layout.serve_root / "files").u8string() +
          " to serve the old files directory instead.");
    }

    fsrv::probe_same_volume_or_die(ctx.serve_root, ctx.tmp_root);
    fsrv::db_init(ctx.db_path);
    ctx.paths = fsrv::PathGuard::make(ctx.serve_root, {ctx.meta_root});
    ctx.started_at = fsrv::now_sec();

    log_banner(ctx);
    if (config_is_public) {
      fsrv::log_warn("configuration file is inside the served directory and can "
                     "be downloaded: " +
                     config_path.u8string());
    }

    const auto scan_start = std::chrono::steady_clock::now();
    const fsrv::RescanStats stats = fsrv::files_rescan(ctx.serve_root);
    const auto scan_ms = std::chrono::duration_cast<std::chrono::milliseconds>(
                             std::chrono::steady_clock::now() - scan_start)
                             .count();
    fsrv::log_info("index scan  : " + std::to_string(stats.scanned) +
                   " entries, " + std::to_string(stats.removed) +
                   " stale rows removed in " + std::to_string(scan_ms) + " ms");

    const int64_t orphans = fsrv::upload_cleanup_orphans(ctx);
    if (orphans > 0) {
      fsrv::log_info("removed " + std::to_string(orphans) +
                     " orphaned upload directorie(s)");
    }
    const int64_t expired = fsrv::upload_cleanup_expired(ctx);
    if (expired > 0) {
      fsrv::log_info("removed " + std::to_string(expired) +
                     " expired upload session(s)");
    }
    const fsrv::PurgeStats purge =
        fsrv::audit_purge(ctx.cfg.audit_retention_days, ctx.cfg.audit_max_size_mb);
    if (purge.deleted_by_age + purge.deleted_by_size > 0) {
      fsrv::log_info("audit purge : " +
                     std::to_string(purge.deleted_by_age + purge.deleted_by_size) +
                     " row(s) removed");
    }

    fsrv::FileServer server(ctx);

    std::signal(SIGINT, on_signal);
    std::signal(SIGTERM, on_signal);

    bool listen_failed = false;
    std::thread server_thread([&] {
      if (!server.listen()) listen_failed = true;
      g_stop_flag = 1;
    });

    std::thread maintenance([&ctx] {
      int minutes = 0;
      while (g_stop_flag == 0) {
        for (int i = 0; i < 60 && g_stop_flag == 0; ++i) {
          std::this_thread::sleep_for(std::chrono::seconds(1));
        }
        if (g_stop_flag != 0) break;
        ++minutes;
        try {
          const int64_t removed = fsrv::upload_cleanup_expired(ctx);
          if (removed > 0) {
            fsrv::log_info("cleanup: removed " + std::to_string(removed) +
                           " expired upload session(s)");
          }
          if (minutes % 10 == 0) {
            fsrv::audit_purge(ctx.cfg.audit_retention_days,
                              ctx.cfg.audit_max_size_mb);
          }
        } catch (const std::exception& e) {
          fsrv::log_error(std::string("maintenance task failed: ") + e.what());
        } catch (...) {
          fsrv::log_error("maintenance task failed: unknown error");
        }
      }
    });

    while (g_stop_flag == 0) {
      std::this_thread::sleep_for(std::chrono::milliseconds(100));
    }

    fsrv::log_info("shutting down");
    server.stop();
    server_thread.join();
    maintenance.join();

    return listen_failed ? 1 : 0;
  } catch (const fsrv::ConfigError& e) {
    std::fprintf(stderr, "configuration error: %s\n", e.what());
    return 2;
  } catch (const fsrv::ApiError& e) {
    std::fprintf(stderr, "startup failed (%s): %s\n", e.code.c_str(), e.what());
    return 1;
  } catch (const std::exception& e) {
    std::fprintf(stderr, "startup failed: %s\n", e.what());
    return 1;
  }
}
