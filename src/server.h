#pragma once

#include <memory>
#include <string>

#include "app_context.h"
#include "hpx.h"

namespace fsrv {

class FileServer {
 public:
  explicit FileServer(const AppContext& ctx);
  ~FileServer();

  FileServer(const FileServer&) = delete;
  FileServer& operator=(const FileServer&) = delete;

  // Binds and blocks while serving. Returns false when the port cannot be
  // bound.
  bool listen();

  void stop();

 private:
  void register_routes();

  const AppContext& ctx_;
  std::unique_ptr<httplib::Server> svr_;
};

}  // namespace fsrv
