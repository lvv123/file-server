#pragma once

#include "compat.h"

namespace fsrv {

// Prevents two server processes from sharing the same metadata directory.
// SQLite can serialise simple writes, but upload sessions and merge operations
// are guarded by in-process locks, so a second instance would race them.
class InstanceLock {
 public:
  explicit InstanceLock(const fs::path& path);
  ~InstanceLock();

  InstanceLock(const InstanceLock&) = delete;
  InstanceLock& operator=(const InstanceLock&) = delete;

 private:
#if defined(_WIN32)
  void* handle_ = nullptr;
#else
  int fd_ = -1;
#endif
};

}  // namespace fsrv
