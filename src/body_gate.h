#pragma once

#include <cstddef>
#include <functional>

#include "hpx.h"
#include "upload.h"

namespace fsrv {

// Guards the single-shot read of a request body.
//
// cpp-httplib hands a content-reader handler a ContentReader that reads from the
// socket on demand: every call consumes the next bytes of the body. Calling it a
// second time once the body is exhausted blocks in recv() until the read
// timeout, which shows up as a hung request rather than an error. Every rejection
// path still has to drain the body before the error response goes out (otherwise
// the leftover bytes desynchronise the keep-alive connection), so exactly one
// read attempt is allowed: BodyGate tracks whether that attempt happened and
// turns any later drain into a no-op.
class BodyGate : public BodySource {
 public:
  explicit BodyGate(const httplib::ContentReader& reader) : reader_(&reader) {}

  // Reads the body, streaming it to `sink`. Only valid once per request.
  bool read(const std::function<bool(const char*, size_t)>& sink) override {
    started_ = true;
    return (*reader_)(sink);
  }

  // Discards whatever part of the body has not been read yet, unless reading
  // has already started (in which case the body is either fully consumed or the
  // connection is already broken).
  void drain() override {
    if (started_) { return; }
    started_ = true;
    (*reader_)([](const char*, size_t) { return true; });
  }

  // Marks the body as handled without reading it. Used when the request has no
  // Content-Length: httplib would then read until EOF, i.e. block in recv()
  // until the client closes the connection, which is never what we want. There
  // is nothing to skip in that case, so simply refusing further reads is both
  // correct and deadlock free.
  void abandon() override { started_ = true; }

  bool started() const { return started_; }

 private:
  const httplib::ContentReader* reader_;
  bool started_ = false;
};

}  // namespace fsrv
