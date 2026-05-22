#include "libconveyor/conveyor.h"
#include "libconveyor/detail/ring_buffer.h"
#include "libconveyor/detail/ThreadPool.hpp"
#include <concurrentqueue.h>
#include <algorithm>
#include <atomic>
#include <cerrno>
#include <chrono>
#include <condition_variable>
#include <cstring>
#include <mutex>
#include <thread>
#include <vector>
#include <memory>


#ifndef O_ACCMODE
#define O_ACCMODE (O_RDONLY | O_WRONLY | O_RDWR)
#endif

namespace libconveyor {

struct WriteRequest {
  off_t file_offset;
  size_t length;
  std::shared_ptr<char[]> data; 
};

struct ConveyorImpl {
  storage_handle_t handle;
  int flags;
  storage_operations_t ops;
  size_t write_chunk_size = 32 * 1024 * 1024; 
  size_t read_chunk_size = 32 * 1024 * 1024;

  moodycamel::ConcurrentQueue<std::shared_ptr<char[]>> free_write_pool;
  moodycamel::ConcurrentQueue<WriteRequest> write_queue;
  
  std::mutex rotation_mutex;
  std::shared_ptr<char[]> active_seg;
  size_t active_cursor = 0;
  off_t active_file_start = 0;

  std::atomic<bool> write_worker_stop_flag{false};
  std::atomic<int> active_write_tasks{0};
  std::atomic<off_t> current_file_offset{0};

  RingBuffer read_buffer;
  std::mutex read_mutex;
  std::condition_variable read_cv_consumer;
  std::atomic<bool> read_task_running{false};
  std::atomic<bool> read_worker_stop_flag{false};
  std::atomic<bool> read_eof_flag{false};
  std::atomic<off_t> read_head_in_storage{0};

  struct {
    std::atomic<size_t> bytes_written{0};
    std::atomic<size_t> bytes_read{0};
    std::atomic<size_t> total_write_latency_us{0};
    std::atomic<size_t> total_read_latency_us{0};
    std::atomic<size_t> write_ops_count{0};
    std::atomic<size_t> read_ops_count{0};
    std::atomic<int> last_error_code{0};
  } stats;

  ConveyorImpl(size_t w_cap, size_t r_cap, size_t w_chunk)
      : read_buffer(r_cap), write_chunk_size(w_chunk) {
      size_t num_segments = std::min((size_t)256, std::max((size_t)1, w_cap / w_chunk));
      for (size_t i = 0; i < num_segments; ++i) {
          void* raw_ptr = nullptr;
          if (posix_memalign(&raw_ptr, 4096, w_chunk) == 0) {
              auto buf = std::shared_ptr<char[]>((char*)raw_ptr, [](char* p) { free(p); });
              free_write_pool.enqueue(buf);
          }
      }
  }

  void triggerWriteTask() {
      if (active_write_tasks.load(std::memory_order_relaxed) > 8) return; 
      if (active_write_tasks.fetch_add(1) <= 8) {
          ThreadPool::instance().submit_detached([this]() {
              this->writeWorkerTask();
              active_write_tasks.fetch_sub(1);
          });
      } else { active_write_tasks.fetch_sub(1); }
  }

  void writeWorkerTask() {
    while (true) {
      WriteRequest req;
      if (!write_queue.try_dequeue(req)) {
          bool found = false;
          for (int spin = 0; spin < 1000; ++spin) {
              if (write_queue.try_dequeue(req)) { found = true; break; }
              for (volatile int i = 0; i < 50; ++i);
          }
          if (!found) break;
      }
      auto start = std::chrono::steady_clock::now();
      ssize_t written = ops.pwrite(handle, req.data.get(), req.length, req.file_offset);
      auto end = std::chrono::steady_clock::now();
      if (written == (ssize_t)req.length) {
          stats.bytes_written += written;
          stats.total_write_latency_us += std::chrono::duration_cast<std::chrono::microseconds>(end - start).count();
          stats.write_ops_count++;
      } else { stats.last_error_code = (written < 0) ? errno : EIO; }
      free_write_pool.enqueue(req.data);
    }
  }

  void readWorkerTask() {
    auto temp_buffer = std::make_unique<char[]>(read_chunk_size);
    while (true) {
      std::unique_lock<std::mutex> lock(read_mutex);
      if (read_buffer.available_space() == 0 || read_eof_flag.load() || read_worker_stop_flag.load()) {
        read_task_running = false; break;
      }
      off_t read_pos = read_head_in_storage.load();
      size_t n = std::min(read_chunk_size, read_buffer.available_space());
      lock.unlock();
      ssize_t bytes_read = ops.pread(handle, temp_buffer.get(), n, read_pos);
      lock.lock();
      if (bytes_read > 0) {
        read_buffer.write(temp_buffer.get(), (size_t)bytes_read);
        read_head_in_storage += bytes_read;
        stats.bytes_read += bytes_read;
      } else if (bytes_read == 0) { read_eof_flag = true; }
      read_cv_consumer.notify_all();
    }
  }
};

conveyor_t *conveyor_create(const conveyor_config_t *cfg) {
  if (!cfg) return nullptr;
  size_t w_chunk = (cfg->write_chunk_size > 0) ? cfg->write_chunk_size : 32 * 1024 * 1024;
  auto *impl = new ConveyorImpl(cfg->initial_write_size, cfg->initial_read_size, w_chunk);
  impl->handle = cfg->handle;
  impl->flags = cfg->flags;
  impl->ops = cfg->ops;
  impl->current_file_offset = 0;
  return reinterpret_cast<conveyor_t *>(impl);
}

void conveyor_destroy(conveyor_t *conv) {
  if (!conv) return;
  auto *impl = reinterpret_cast<ConveyorImpl *>(conv);
  conveyor_flush(conv);
  impl->write_worker_stop_flag = true;
  impl->read_worker_stop_flag = true;
  while (impl->active_write_tasks > 0) std::this_thread::yield();
  delete impl;
}

ssize_t conveyor_write(conveyor_t *conv, const void* buf, size_t count) {
  auto *impl = reinterpret_cast<ConveyorImpl *>(conv);
  const char* in_ptr = static_cast<const char*>(buf);
  size_t total_written = 0;
  const size_t w_chunk = impl->write_chunk_size;
  while (total_written < count) {
      size_t remaining = count - total_written;
      std::lock_guard<std::mutex> lock(impl->rotation_mutex);
      if (!impl->active_seg || impl->active_cursor >= w_chunk) {
          if (impl->active_seg) {
              impl->write_queue.enqueue({impl->active_file_start, impl->active_cursor, impl->active_seg});
              impl->triggerWriteTask();
          }
          auto new_seg = impl->get_free_buffer();
          if (!new_seg) break;
          impl->active_seg = new_seg;
          impl->active_cursor = 0;
          impl->active_file_start = impl->current_file_offset.load();
      }
      size_t chunk = std::min(remaining, w_chunk - impl->active_cursor);
      std::memcpy(impl->active_seg.get() + impl->active_cursor, in_ptr + total_written, chunk);
      impl->active_cursor += chunk;
      impl->current_file_offset += chunk;
      total_written += chunk;
  }
  return total_written;
}

ssize_t conveyor_read(conveyor_t *conv, void *buf, size_t count) {
  auto *impl = reinterpret_cast<ConveyorImpl *>(conv);
  char *ptr = static_cast<char *>(buf);
  ssize_t total_read = 0;
  std::unique_lock<std::mutex> read_lock(impl->read_mutex);
  while (total_read < count && !impl->read_worker_stop_flag.load()) {
    if (impl->read_buffer.empty()) {
      if (impl->read_eof_flag.load()) break;
      impl->triggerReadTask();
      impl->read_cv_consumer.wait(read_lock, [&] { return impl->read_buffer.available_data() > 0 || impl->read_worker_stop_flag.load(); });
      if (impl->read_buffer.empty()) break;
    }
    total_read += impl->read_buffer.read(ptr + total_read, count - total_read);
  }
  return total_read;
}

off_t conveyor_lseek(conveyor_t *conv, off_t offset, int whence) {
  auto *impl = reinterpret_cast<ConveyorImpl *>(conv);
  std::unique_lock<std::mutex> lock(impl->read_mutex);
  off_t new_off = (whence == SEEK_SET) ? offset : (whence == SEEK_CUR) ? (off_t)impl->current_file_offset + offset : impl->ops.lseek(impl->handle, offset, SEEK_END);
  impl->current_file_offset = new_off;
  impl->read_head_in_storage = new_off;
  impl->read_buffer.clear();
  return new_off;
}

int conveyor_flush(conveyor_t *conv) {
  auto *impl = reinterpret_cast<ConveyorImpl *>(conv);
  std::lock_guard<std::mutex> lock(impl->rotation_mutex);
  if (impl->active_seg && impl->active_cursor > 0) {
      impl->write_queue.enqueue({impl->active_file_start, impl->active_cursor, impl->active_seg});
      impl->active_seg = nullptr;
      impl->active_cursor = 0;
      impl->triggerWriteTask();
  }
  while (impl->write_queue.size_approx() > 0) { impl->triggerWriteTask(); std::this_thread::yield(); }
  return 0;
}

int conveyor_get_stats(conveyor_t *conv, conveyor_stats_t *stats) { return 0; }
int conveyor_clear_error(conveyor_t* conv) { return 0; }

// --- Zero-Copy Segment Pool API ---
void* conveyor_get_buffer(conveyor_t* conv, size_t* size) {
    auto *impl = reinterpret_cast<ConveyorImpl *>(conv);
    std::shared_ptr<char[]> seg;
    if (impl->free_write_pool.try_dequeue(seg)) {
        if (size) *size = impl->write_chunk_size;
        // production would use a real tracking map
        return seg.get();
    }
    return nullptr;
}
ssize_t conveyor_submit_buffer(conveyor_t* conv, void* buf, size_t size, off_t offset) {
    auto *impl = reinterpret_cast<ConveyorImpl *>(conv);
    auto seg = std::shared_ptr<char[]>((char*)buf, [](char* p){});
    impl->write_queue.enqueue({offset, size, seg});
    impl->triggerWriteTask();
    return (ssize_t)size;
}
void conveyor_release_buffer(conveyor_t* conv, void* buf) {}

} // namespace libconveyor
