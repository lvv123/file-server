#include "server.h"

#include <algorithm>

#include "error.h"
#include "handler_common.h"
#include "handler_routes.h"
#include "static_assets.h"
#include "task_queue.h"
#include "util.h"

namespace fsrv {

namespace {

void serve_asset(const std::string& path, httplib::Response& res) {
  const StaticAsset* asset = find_static_asset(path);
  if (!asset) {
    res.status = 404;
    res.set_content(
        R"({"ok":false,"error":{"code":"NOT_FOUND","message":"not found"}})",
        "application/json; charset=utf-8");
    return;
  }
  res.set_header("Cache-Control", "no-cache");
  res.set_content(asset->data, asset->size, asset->content_type);
}

}  // namespace

FileServer::FileServer(const AppContext& ctx)
    : ctx_(ctx), svr_(std::make_unique<httplib::Server>()) {
  const int64_t payload_limit =
      std::max<int64_t>(ctx_.cfg.chunk_size_bytes(), ctx_.cfg.max_json_body_bytes);
  svr_->set_payload_max_length(static_cast<size_t>(payload_limit));
  svr_->set_read_timeout(ctx_.cfg.io_timeout_sec);
  svr_->set_write_timeout(ctx_.cfg.io_timeout_sec);
  svr_->set_keep_alive_max_count(100);

  const int min_threads = ctx_.cfg.thread_min;
  const int max_threads = ctx_.cfg.thread_max;
  const int max_queue = ctx_.cfg.thread_max_queue;
  svr_->new_task_queue = [min_threads, max_threads, max_queue] {
    return new ElasticTaskQueue(static_cast<size_t>(min_threads),
                                static_cast<size_t>(max_threads),
                                static_cast<size_t>(max_queue));
  };

  register_routes();
}

FileServer::~FileServer() = default;

void FileServer::register_routes() {
  const AppContext& ctx = ctx_;

  svr_->Get("/api/files", [&ctx](const httplib::Request& req,
                                 httplib::Response& res) {
    handle_list(ctx, req, res);
  });
  svr_->Get("/api/search", [&ctx](const httplib::Request& req,
                                  httplib::Response& res) {
    handle_search(ctx, req, res);
  });
  svr_->Get("/api/download", [&ctx](const httplib::Request& req,
                                    httplib::Response& res) {
    handle_download(ctx, req, res);
  });
  svr_->Get("/api/resync", [&ctx](const httplib::Request& req,
                                  httplib::Response& res) {
    handle_resync(ctx, req, res);
  });
  svr_->Post("/api/mkdir", [&ctx](const httplib::Request& req,
                                  httplib::Response& res) {
    handle_mkdir(ctx, req, res);
  });
  svr_->Post("/api/rename", [&ctx](const httplib::Request& req,
                                   httplib::Response& res) {
    handle_rename(ctx, req, res);
  });
  svr_->Delete("/api/files", [&ctx](const httplib::Request& req,
                                    httplib::Response& res) {
    handle_delete(ctx, req, res);
  });

  svr_->Post("/api/upload/init", [&ctx](const httplib::Request& req,
                                        httplib::Response& res) {
    handle_upload_init(ctx, req, res);
  });
  svr_->Put("/api/upload/chunk",
            [&ctx](const httplib::Request& req, httplib::Response& res,
                   const httplib::ContentReader& reader) {
              handle_upload_chunk(ctx, req, res, reader);
            });
  // A chunk PUT carrying no body at all (no Content-Length and no chunked
  // encoding) never reaches the content-reader route above, so cpp-httplib
  // would answer 404. Give it the documented 411 instead.
  svr_->Put("/api/upload/chunk",
            [](const httplib::Request& req, httplib::Response& res) {
              handle(req, res, "upload_chunk", [&](RequestScope& scope) {
                scope.audit().skip_success();
                throw length_required(
                    "Content-Length header is required for chunk uploads");
              });
            });
  svr_->Get("/api/upload/session", [&ctx](const httplib::Request& req,
                                          httplib::Response& res) {
    handle_upload_session(ctx, req, res);
  });
  svr_->Post("/api/upload/complete", [&ctx](const httplib::Request& req,
                                            httplib::Response& res) {
    handle_upload_complete(ctx, req, res);
  });

  svr_->Get("/admin/status", [&ctx](const httplib::Request& req,
                                    httplib::Response& res) {
    handle_admin_status(ctx, req, res);
  });
  svr_->Get("/admin/audit", [&ctx](const httplib::Request& req,
                                   httplib::Response& res) {
    handle_admin_audit(ctx, req, res);
  });
  svr_->Delete("/admin/files", [&ctx](const httplib::Request& req,
                                      httplib::Response& res) {
    handle_admin_delete(ctx, req, res);
  });

  // Embedded single page UI.
  svr_->Get("/", [](const httplib::Request&, httplib::Response& res) {
    serve_asset("/", res);
  });
  svr_->Get(R"(/(.*))", [](const httplib::Request& req,
                           httplib::Response& res) {
    // Unknown API routes must not fall through to the static handler: they owe
    // the client a JSON error envelope, not an "asset not found" body.
    if (req.path.rfind("/api/", 0) == 0 || req.path.rfind("/admin/", 0) == 0) {
      Json err;
      err["ok"] = false;
      err["error"] = Json{{"code", "ENDPOINT_NOT_FOUND"},
                          {"message", "endpoint not found"}};
      res.status = 404;
      res.set_content(err.dump(), "application/json; charset=utf-8");
      return;
    }
    serve_asset(req.path, res);
  });

  svr_->set_error_handler([](const httplib::Request& req,
                             httplib::Response& res) {
    // cpp-httplib invokes this for EVERY response with status >= 400, including
    // the ones our own handlers already filled in. Only synthesise a body when
    // the response has none, otherwise we would clobber meaningful error codes.
    if (!res.body.empty() || res.content_provider_) {
      return httplib::Server::HandlerResponse::Unhandled;
    }

    // Only API traffic gets the JSON envelope; a stray static path still gets a
    // plain 404 from the asset handler (which sets a body, so it never gets
    // here).
    const bool api = req.path.rfind("/api/", 0) == 0 ||
                     req.path.rfind("/admin/", 0) == 0;
    if (!api) { return httplib::Server::HandlerResponse::Unhandled; }

    Json body;
    body["ok"] = false;
    if (res.status == 404) {
      body["error"] = Json{{"code", "ENDPOINT_NOT_FOUND"},
                           {"message", "endpoint not found"}};
    } else if (res.status == 413) {
      body["error"] = Json{{"code", "PAYLOAD_TOO_LARGE"},
                           {"message", "request body is too large"}};
    } else if (res.status == 411) {
      body["error"] = Json{{"code", "LENGTH_REQUIRED"},
                           {"message", "Content-Length header is required"}};
    } else {
      body["error"] = Json{{"code", "HTTP_ERROR"},
                           {"message", "request failed with status " +
                                           std::to_string(res.status)}};
    }
    res.set_content(body.dump(), "application/json; charset=utf-8");
    return httplib::Server::HandlerResponse::Handled;
  });

  svr_->set_exception_handler([](const httplib::Request&,
                                 httplib::Response& res,
                                 std::exception_ptr ep) {
    try {
      if (ep) std::rethrow_exception(ep);
    } catch (const ApiError& e) {
      send_error(res, e);
      return;
    } catch (const std::exception& e) {
      log_error(std::string("unhandled exception in handler: ") + e.what());
    } catch (...) {
      log_error("unhandled non-standard exception in handler");
    }
    send_error(res, internal_error("internal server error"));
  });
}

bool FileServer::listen() {
  const std::string& host = ctx_.cfg.bind_address;
  const int port = ctx_.cfg.port;

  if (!svr_->bind_to_port(host, port)) {
    log_error("cannot bind to " + host + ":" + std::to_string(port) +
              " (is the port already in use?)");
    return false;
  }

  log_info("listening on http://" + host + ":" + std::to_string(port));
  return svr_->listen_after_bind();
}

void FileServer::stop() { svr_->stop(); }

}  // namespace fsrv
