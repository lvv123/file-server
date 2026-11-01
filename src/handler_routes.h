#pragma once

#include "app_context.h"
#include "handler_common.h"
#include "hpx.h"

namespace fsrv {

// ---- business API ------------------------------------------------------
void handle_list(const AppContext& ctx, const httplib::Request& req,
                 httplib::Response& res);
void handle_search(const AppContext& ctx, const httplib::Request& req,
                   httplib::Response& res);
void handle_mkdir(const AppContext& ctx, const httplib::Request& req,
                  httplib::Response& res);
void handle_rename(const AppContext& ctx, const httplib::Request& req,
                   httplib::Response& res);
void handle_delete(const AppContext& ctx, const httplib::Request& req,
                   httplib::Response& res);
// Rebuilds the file index from what is actually on disk.
void handle_resync(const AppContext& ctx, const httplib::Request& req,
                   httplib::Response& res);

// Shared implementation used by both the business and the admin delete routes.
void perform_delete(const AppContext& ctx, const httplib::Request& req,
                    httplib::Response& res, RequestScope& scope);
void handle_download(const AppContext& ctx, const httplib::Request& req,
                     httplib::Response& res);

// ---- upload API --------------------------------------------------------
void handle_upload_init(const AppContext& ctx, const httplib::Request& req,
                        httplib::Response& res);
void handle_upload_chunk(const AppContext& ctx, const httplib::Request& req,
                         httplib::Response& res,
                         const httplib::ContentReader& reader);
void handle_upload_session(const AppContext& ctx, const httplib::Request& req,
                           httplib::Response& res);
void handle_upload_complete(const AppContext& ctx, const httplib::Request& req,
                            httplib::Response& res);

// ---- admin API ---------------------------------------------------------
bool is_admin_client(const Config& cfg, const std::string& ip);
void handle_admin_status(const AppContext& ctx, const httplib::Request& req,
                         httplib::Response& res);
void handle_admin_audit(const AppContext& ctx, const httplib::Request& req,
                        httplib::Response& res);
void handle_admin_delete(const AppContext& ctx, const httplib::Request& req,
                         httplib::Response& res);

}  // namespace fsrv
