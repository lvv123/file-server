#pragma once

// cpp-httplib include wrapper with third-party warnings suppressed.
#if defined(_MSC_VER)
#pragma warning(push)
#pragma warning(disable : 4244 4267 4018 4100 4456 4457 4458 4459 4702 4996)
#endif
#include "httplib.h"
#if defined(_MSC_VER)
#pragma warning(pop)
#endif
