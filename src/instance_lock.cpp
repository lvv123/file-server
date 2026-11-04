#include "instance_lock.h"

#include "error.h"

#if defined(_WIN32)
#include <windows.h>
#else
#include <fcntl.h>
#include <sys/file.h>
#include <unistd.h>
#endif

namespace fsrv {

InstanceLock::InstanceLock(const fs::path& path) {
#if defined(_WIN32)
  HANDLE handle = CreateFileW(path.wstring().c_str(), GENERIC_READ | GENERIC_WRITE,
                              0, nullptr, OPEN_ALWAYS, FILE_ATTRIBUTE_NORMAL,
                              nullptr);
  if (handle == INVALID_HANDLE_VALUE) {
    const DWORD error = GetLastError();
    if (error == ERROR_SHARING_VIOLATION) {
      throw internal_error(
          "another file-server instance is already using this directory",
          "INSTANCE_LOCKED");
    }
    throw internal_error("cannot lock the metadata directory: " +
                             path.u8string() + " (error " +
                             std::to_string(error) + ")",
                         "INSTANCE_LOCK_FAILED");
  }
  handle_ = handle;
#else
  const int fd = ::open(path.c_str(), O_CREAT | O_RDWR, 0644);
  if (fd < 0) {
    throw internal_error("cannot open lock file: " + path.u8string(),
                         "INSTANCE_LOCK_FAILED");
  }
  if (::flock(fd, LOCK_EX | LOCK_NB) != 0) {
    ::close(fd);
    throw internal_error(
        "another file-server instance is already using this directory",
        "INSTANCE_LOCKED");
  }
  fd_ = fd;
#endif
}

InstanceLock::~InstanceLock() {
#if defined(_WIN32)
  if (handle_ != nullptr) {
    CloseHandle(static_cast<HANDLE>(handle_));
  }
#else
  if (fd_ >= 0) {
    ::flock(fd_, LOCK_UN);
    ::close(fd_);
  }
#endif
}

}  // namespace fsrv
