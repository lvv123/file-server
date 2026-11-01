#include <algorithm>
#include <system_error>

#include "download.h"
#include "error.h"
#include "fsops.h"
#include "handler_routes.h"
#include "repo_files.h"
#include "util.h"

namespace fsrv {

namespace {

Json entry_json(const FileRow& row) {
  Json j;
  j["name"] = row.name;
  j["path"] = "/" + row.rel_path;
  j["type"] = row.is_dir ? "dir" : "file";
  j["is_dir"] = row.is_dir;
  j["size"] = row.is_dir ? 0 : row.size;
  j["mtime"] = row.mtime;
  j["mtime_text"] = format_time_iso(row.mtime);
  return j;
}

}  // namespace

void handle_list(const AppContext& ctx, const httplib::Request& req,
                 httplib::Response& res) {
  handle(req, res, "list", [&](RequestScope& scope) {
    const std::string user_path = req.get_param_value("path");
    const fs::path dir = ctx.paths.resolve(user_path);
    const std::string rel = ctx.paths.to_rel(dir);

    bool is_dir = false;
    int64_t size = 0;
    int64_t mtime = 0;
    if (!stat_path(dir, is_dir, size, mtime)) {
      throw not_found("directory not found", "DIR_NOT_FOUND");
    }
    if (!is_dir) {
      throw bad_request("path is a file, not a directory", "TARGET_IS_FILE");
    }
    scope.set_path(rel);

    Json data;
    data["path"] = rel.empty() ? "/" : "/" + rel;
    data["entries"] = Json::array();
    for (const auto& row : files_list(rel)) {
      data["entries"].push_back(entry_json(row));
    }
    scope.ok(std::move(data));
  });
}

void handle_search(const AppContext& ctx, const httplib::Request& req,
                   httplib::Response& res) {
  handle(req, res, "search", [&](RequestScope& scope) {
    std::string q = req.get_param_value("q");
    if (q.empty()) {
      throw bad_request("query parameter 'q' is required", "MISSING_QUERY");
    }
    scope.set_path(q);

    int limit = ctx.cfg.search_limit_default;
    const std::string limit_param = req.get_param_value("limit");
    if (!limit_param.empty()) {
      try {
        limit = std::stoi(limit_param);
      } catch (...) {
        throw bad_request("limit must be an integer", "INVALID_LIMIT");
      }
    }
    if (limit < 1) limit = 1;
    if (limit > ctx.cfg.search_limit_max) limit = ctx.cfg.search_limit_max;

    int offset = 0;
    const std::string offset_param = req.get_param_value("offset");
    if (!offset_param.empty()) {
      try {
        offset = std::stoi(offset_param);
      } catch (...) {
        throw bad_request("offset must be an integer", "INVALID_OFFSET");
      }
    }
    if (offset < 0) offset = 0;

    Json data;
    data["query"] = q;
    data["limit"] = limit;
    data["offset"] = offset;
    data["results"] = Json::array();
    for (const auto& hit : files_search(q, limit, offset)) {
      Json item;
      item["name"] = hit.name;
      item["path"] = "/" + hit.rel_path;
      item["size"] = hit.size;
      item["mtime"] = hit.mtime;
      item["mtime_text"] = format_time_iso(hit.mtime);
      data["results"].push_back(std::move(item));
    }
    scope.ok(std::move(data));
  });
}

void handle_mkdir(const AppContext& ctx, const httplib::Request& req,
                  httplib::Response& res) {
  handle(req, res, "mkdir", [&](RequestScope& scope) {
    const Json body = parse_json_body(req);
    const std::string user_path = require_string_field(body, "path");
    const fs::path dir = ctx.paths.resolve(user_path);
    const std::string rel = ctx.paths.to_rel(dir);
    scope.set_path(rel);

    if (rel.empty()) {
      throw conflict("directory already exists", "TARGET_EXISTS");
    }

    std::error_code ec;
    if (fs::exists(dir, ec)) {
      bool existing_is_dir = false;
      int64_t existing_size = 0;
      int64_t existing_mtime = 0;
      stat_path(dir, existing_is_dir, existing_size, existing_mtime);
      if (!existing_is_dir) {
        throw conflict("a file with the same name already exists",
                       "TARGET_IS_FILE");
      }
      Json data;
      data["path"] = "/" + rel;
      data["created"] = false;
      scope.ok(std::move(data));
      return;
    }

    files_make_dirs(ctx.files_root, rel);

    Json data;
    data["path"] = "/" + rel;
    data["created"] = true;
    scope.ok(std::move(data));
  });
}

void handle_rename(const AppContext& ctx, const httplib::Request& req,
                   httplib::Response& res) {
  handle(req, res, "rename", [&](RequestScope& scope) {
    const Json body = parse_json_body(req);
    const std::string from_user = require_string_field(body, "from");
    const std::string to_user = require_string_field(body, "to");

    const fs::path from = ctx.paths.resolve(from_user);
    const fs::path to = ctx.paths.resolve(to_user);
    const std::string from_rel = ctx.paths.to_rel(from);
    const std::string to_rel = ctx.paths.to_rel(to);
    scope.set_path(from_rel);

    if (from_rel.empty() || to_rel.empty()) {
      throw forbidden("the server root cannot be renamed", "ROOT_IMMUTABLE");
    }
    if (from_rel == to_rel) {
      throw conflict("source and destination are identical", "SAME_PATH");
    }

    std::error_code ec;
    if (!fs::exists(from, ec)) {
      throw not_found("source path does not exist", "SOURCE_NOT_FOUND");
    }
    if (fs::exists(to, ec)) {
      throw conflict("destination already exists", "TARGET_EXISTS");
    }

    bool is_dir = false;
    int64_t size = 0;
    int64_t mtime = 0;
    stat_path(from, is_dir, size, mtime);
    if (is_dir && to_rel.rfind(from_rel + "/", 0) == 0) {
      throw bad_request("cannot move a directory into itself",
                        "INVALID_DESTINATION");
    }

    fs::rename(from, to, ec);
    if (ec) {
      throw io_error_from("cannot rename '" + from_rel + "' to '" + to_rel + "'",
                          ec);
    }

    files_rename_tree(from_rel, to_rel);

    Json data;
    data["from"] = "/" + from_rel;
    data["to"] = "/" + to_rel;
    scope.ok(std::move(data));
  });
}

void handle_delete(const AppContext& ctx, const httplib::Request& req,
                   httplib::Response& res) {
  handle(req, res, "delete", [&](RequestScope& scope) {
    perform_delete(ctx, req, res, scope);
  });
}

void perform_delete(const AppContext& ctx, const httplib::Request& req,
                    httplib::Response& res, RequestScope& scope) {
  (void)res;
  {
    const fs::path target = ctx.paths.resolve(req.get_param_value("path"));
    const std::string rel = ctx.paths.to_rel(target);
    scope.set_path(rel);

    if (rel.empty()) {
      throw forbidden("the server root cannot be deleted", "ROOT_IMMUTABLE");
    }

    bool is_dir = false;
    int64_t size = 0;
    int64_t mtime = 0;
    if (!stat_path(target, is_dir, size, mtime)) {
      throw not_found("path does not exist", "NOT_FOUND");
    }
    scope.set_size(is_dir ? std::nullopt : std::optional<int64_t>(size));

    std::error_code ec;
    if (is_dir && !fs::is_empty(target, ec)) {
      throw conflict("directory is not empty", "DIRECTORY_NOT_EMPTY");
    }

    fs::remove(target, ec);
    if (ec) {
      throw io_error_from("cannot delete '" + rel + "'", ec);
    }

    files_remove_tree(rel);

    Json data;
    data["path"] = "/" + rel;
    data["deleted"] = true;
    scope.ok(std::move(data));
  }
}

void handle_download(const AppContext& ctx, const httplib::Request& req,
                     httplib::Response& res) {
  handle(req, res, "download", [&](RequestScope& scope) {
    download_file(ctx, req, res, scope);
  });
}

}  // namespace fsrv
