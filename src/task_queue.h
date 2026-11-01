#pragma once

#include <algorithm>
#include <condition_variable>
#include <cstddef>
#include <deque>
#include <functional>
#include <mutex>
#include <thread>
#include <vector>

#include "hpx.h"

namespace fsrv {

// Task queue used by cpp-httplib.
//
// cpp-httplib 0.18's built-in ThreadPool has a fixed size and no notion of a
// maximum number of threads, while the specification asks for a pool that
// starts at `min_threads`, grows to `max_threads` under load and rejects work
// once `max_queue` requests are waiting.
class ElasticTaskQueue final : public httplib::TaskQueue {
 public:
  ElasticTaskQueue(size_t min_threads, size_t max_threads, size_t max_queue)
      : min_threads_(min_threads == 0 ? 1 : min_threads),
        max_threads_(std::max(min_threads, max_threads)),
        max_queue_(max_queue) {
    std::lock_guard<std::mutex> lk(mutex_);
    spawn_locked(min_threads_);
  }

  ~ElasticTaskQueue() override { shutdown(); }

  ElasticTaskQueue(const ElasticTaskQueue&) = delete;
  ElasticTaskQueue& operator=(const ElasticTaskQueue&) = delete;

  bool enqueue(std::function<void()> fn) override {
    {
      std::lock_guard<std::mutex> lk(mutex_);
      if (shutdown_) return false;
      if (max_queue_ > 0 && jobs_.size() >= max_queue_) return false;
      jobs_.push_back(std::move(fn));
      // Every worker is busy and work is piling up: add a thread.
      if (idle_ == 0 && threads_.size() < max_threads_) {
        spawn_locked(1);
      }
    }
    cv_.notify_one();
    return true;
  }

  void shutdown() override {
    {
      std::lock_guard<std::mutex> lk(mutex_);
      if (shutdown_) return;
      shutdown_ = true;
    }
    cv_.notify_all();
    for (auto& thread : threads_) {
      if (thread.joinable()) thread.join();
    }
    threads_.clear();
  }

 private:
  void spawn_locked(size_t count) {
    for (size_t i = 0; i < count; ++i) {
      threads_.emplace_back([this] { worker_loop(); });
    }
  }

  void worker_loop() {
    for (;;) {
      std::function<void()> job;
      {
        std::unique_lock<std::mutex> lk(mutex_);
        ++idle_;
        cv_.wait(lk, [this] { return shutdown_ || !jobs_.empty(); });
        --idle_;
        if (shutdown_ && jobs_.empty()) return;
        job = std::move(jobs_.front());
        jobs_.pop_front();
      }
      job();
    }
  }

  size_t min_threads_;
  size_t max_threads_;
  size_t max_queue_;

  std::mutex mutex_;
  std::condition_variable cv_;
  std::deque<std::function<void()>> jobs_;
  std::vector<std::thread> threads_;
  size_t idle_ = 0;
  bool shutdown_ = false;
};

}  // namespace fsrv
