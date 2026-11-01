#pragma once

// Shared third-party include helper. Warnings from vendored headers are
// suppressed so the project itself can be built with /W4 -Wall -Wextra.
#if defined(_MSC_VER)
#pragma warning(push)
#pragma warning(disable : 4244 4267 4018 4100 4456 4457 4458 4459 4702 4996 5054)
#endif
#include "json.hpp"
#if defined(_MSC_VER)
#pragma warning(pop)
#endif

namespace fsrv {
using Json = nlohmann::json;
}
