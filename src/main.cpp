#include <csignal>
#include <cstdio>
#include <cstring>
#include <string>
#include <thread>

#include "app_context.h"
#include "config.h"
#include "db.h"
#include "error.h"
#include "fsops.h"
#include "paths.h"
#include "repo_audit.h"
#include "repo_files.h"
#include "server.h"
#include "upload.h"
#include "util.h"

namespace {

volatile std::sig_atomic_t g_stop_flag = 0;

void on_signal(int /*signum*/) { g_stop_flag = 1; }

void print_usage() {
  std::printf(
      "file-server - minimalist LAN file server\n"
      "\n"
      "Usage: file-server [options]\n"
      "\n"
      "Options:\n"
      "  -c, --config <path>   Path to the JSON configuration (default: "
      "config.json)\n"
      "      --check-config    Validate the configuration and data root, then "
      "exit\n"
      "  -h, --help            Show this help\n"
      "      --version         Show the version\n");
}

void print_version() { std::printf("file-server 1.0.0\n"); }

void ensure_directories(const fsrv::AppContext& ctx) {
  std::error_code ec;
  fsrv::fs::create_directories(ctx.files_root, ec);
  if (ec) {
    throw fsrv::internal_error("cannot create files directory '" +
                                   ctx.files_root.u8string() + "': " +
                                   ec.message(),
                               "DATA_ROOT_UNUSABLE");
  }
  fsrv::fs::create_directories(ctx.tmp_root, ec);
  if (ec) {
    throw fsrv::internal_error("cannot create tmp directory '" +
                                   ctx.tmp_root.u8string() + "': " +
                                   ec.message(),
                               "DATA_ROOT_UNUSABLE");
  }
}

void log_banner(const fsrv::AppContext& ctx) {
  fsrv::log_info("data root   : " + ctx.data_root.u8string());
  fsrv::log_info("files root  : " + ctx.files_root.u8string());
  fsrv::log_info("tmp root    : " + ctx.tmp_root.u8string());
  fsrv::log_info("database    : " + ctx.db_path.u8string());
  fsrv::log_info("chunk size  : " + std::to_string(ctx.cfg.chunk_size_mb) + " MiB");
  fsrv::log_info("session TTL : " + std::to_string(ctx.cfg.session_ttl_hours) + " h");
  std::string admin_ips;
  for (const auto& ip : ctx.cfg.admin_allow_ips) {
    if (!admin_ips.empty()) admin_ips += ", ";
    admin_ips += ip;
  }
  fsrv::log_info("admin IPs   : " + admin_ips);
}

}  // namespace

int main(int argc, char** argv) {
  std::string config_path = "config.json";
  bool check_only = false;

  for (int i = 1; i < argc; ++i) {
    const std::string arg = argv[i];
    if (arg == "-c" || arg == "--config") {
      if (i + 1 >= argc) {
        std::fprintf(stderr, "error: %s requires a path\n", arg.c_str());
        return 2;
      }
      config_path = argv[++i];
    } else if (arg == "-h" || arg == "--help") {
      print_usage();
      return 0;
    } else if (arg == "--version") {
      print_version();
      return 0;
    } else if (arg == "--check-config") {
      check_only = true;
    } else {
      std::fprintf(stderr, "error: unknown option '%s'\n", arg.c_str());
      print_usage();
      return 2;
    }
  }

  fsrv::AppContext ctx;
  try {
    ctx.cfg = fsrv::load_config(fsrv::fs::u8path(config_path));
    ctx.config_path = config_path;
    ctx.data_root = fsrv::resolve_data_root(ctx.cfg);
    ctx.files_root = ctx.data_root / "files";
    ctx.tmp_root = ctx.data_root / "tmp";
    ctx.db_path = ctx.data_root / "meta.db";

    ensure_directories(ctx);
    fsrv::probe_same_volume_or_die(ctx.files_root, ctx.tmp_root);
    fsrv::db_init(ctx.db_path);
    ctx.paths = fsrv::PathGuard::make(ctx.files_root);
    ctx.started_at = fsrv::now_sec();

    log_banner(ctx);

    const fsrv::RescanStats stats = fsrv::files_rescan(ctx.files_root);
    fsrv::log_info("index scan  : " + std::to_string(stats.scanned) +
                   " entries, " + std::to_string(stats.removed) +
                   " stale rows removed");

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

    if (check_only) {
      fsrv::log_info("configuration check passed");
      return 0;
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
