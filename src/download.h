#pragma once

#include "app_context.h"
#include "handler_common.h"
#include "hpx.h"

namespace fsrv {

// Streams a file with optional single-range support. Registers a
// ContentProvider so the file is never read into memory as a whole.
void download_file(const AppContext& ctx, const httplib::Request& req,
                   httplib::Response& res, RequestScope& scope);

}  // namespace fsrv
