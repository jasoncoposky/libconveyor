#include "libconveyor/conveyor.h"
#include "libconveyor/detail/ring_buffer.h"
#include "libconveyor/detail/ThreadPool.hpp"
#include <concurrentqueue.h>
#include <algorithm>
#include <atomic>
#include <cerrno>
#include <chrono>
#include <condition_variable>
#include <cstring> // For memcpy
#include <mutex>
#include <thread>
#include <vector>
#include <cstdio>
#include <fcntl.h>
#include <memory>


#ifndef O_ACCMODE
#define O_ACCMODE (O_RDONLY | O_WRONLY | O_RDWR)
#endif

namespace libconveyor {

// --- OPTIMIZATION 1: Lightweight Metadata Struct ---
struct WriteRequest {
  off_t file_offset;
  size_t length;
  std::shared_ptr<char[]> data; // Take ownership
};

struct ConveyorImpl {
  storage_handle_t handle;
  int flags;
  storage_operations_t ops;

  size_t max_write_capacity = 0;
  size_t max_read_capacity = 0;

  size_t write_chunk_size = 4 * 1024 * 1024; // Defaults
  size_t read_chunk_size = 4 * 1024 * 1024;

  // Write Logic
  bool write_buffer_enabled = false;
  moodycamel::ConcurrentQueue<WriteRequest> write_queue;
  moodycamel::ConcurrentQueue<std::shared_ptr<char[]>> free_write_pool;
  std::atomic<size_t> pool_size{0};

  std::atomic<bool> write_task_running{false};
  std::atomic<bool> write_worker_stop_flag{false};
  std::atomic<int> active_write_tasks{0};

  std::atomic<off_t> current_file_offset{0};
  std::atomic<off_t> logical_write_offset{0}; // For O_APPEND

  // Read Logic
  bool read_buffer_enabled = false;
  RingBuffer read_buffer;
  std::mutex read_mutex;
  std::condition_variable read_cv_consumer;
  std::atomic<bool> read_task_running{false};
  std::atomic<bool> read_worker_stop_flag{false};
  std::atomic<bool> read_worker_needs_fill{false};
  std::atomic<bool> read_eof_flag{false};
  std::atomic<off_t> read_head_in_storage{0};
  std::atomic<uint64_t> read_buffer_generation{0};
  
  off_t last_read_end_offset = -1;
  int sequential_read_counter = 0;

  // Stats
  struct {
    std::atomic<size_t> bytes_written{0};
    std::atomic<size_t> bytes_read{0};
    std::atomic<size_t> total_write_latency_us{0};
    std::atomic<size_t> total_read_latency_us{0};
    std::atomic<size_t> write_ops_count{0};
    std::atomic<size_t> read_ops_count{0};
    std::atomic<size_t> write_buffer_full_events{0};
    std::atomic<int> last_error_code{0};
  } stats;

  ConveyorImpl(size_t w_cap, size_t r_cap)
      : read_buffer(r_cap), max_write_capacity(w_cap),
        max_read_capacity(r_cap) {}

  std::shared_ptr<char[]> get_free_buffer() {
      std::shared_ptr<char[]> buf;
      if (free_write_pool.try_dequeue(buf)) return buf;
      
      // Alloc new if under capacity
      if (pool_size.load() < 64) { // Max 64 segments in pool
          pool_size++;
          return std::shared_ptr<char[]>(new char[write_chunk_size]);
      }
      
      // Block/Spin for free buffer
      for (int spin = 0; spin < 10000; ++spin) {
          if (free_write_pool.try_dequeue(buf)) return buf;
          for (volatile int i = 0; i < 100; ++i);
      }
      return nullptr; // Should increase pool or block harder
  }

  void triggerWriteTask() {
      if (write_worker_stop_flag.load(std::memory_order_relaxed)) return;
      if (active_write_tasks.load() > 4) return; // Scale up to 4 concurrent writers

      active_write_tasks++;
      ThreadPool::instance().submit_detached([this]() {
          this->writeWorkerTask();
          active_write_tasks--;
      });
  }

  void triggerReadTask() {
      if (read_worker_stop_flag.load(std::memory_order_relaxed)) return;
      if (read_task_running.load(std::memory_order_relaxed)) return;
      if (read_task_running.exchange(true)) return;

      ThreadPool::instance().submit_detached([this]() {
          this->readWorkerTask();
      });
  }

  void writeWorkerTask() {
    while (true) {
      WriteRequest req;
      if (!write_queue.try_dequeue(req)) {
          // Adaptive Spin
          bool found = false;
          for (int spin = 0; spin < 5000; ++spin) {
              if (write_queue.try_dequeue(req)) { found = true; break; }
              for (volatile int i = 0; i < 100; ++i);
          }
          if (!found) break;
      }

      off_t write_pos = (flags & O_APPEND) ? logical_write_offset.load() : req.file_offset;
      auto start = std::chrono::steady_clock::now();
      
      ssize_t written = ops.pwrite(handle, req.data.get(), req.length, write_pos);
      auto end = std::chrono::steady_clock::now();

      if (written == (ssize_t)req.length) {
          stats.bytes_written += written;
          stats.total_write_latency_us += std::chrono::duration_cast<std::chrono::microseconds>(end - start).count();
          stats.write_ops_count++;
          if (flags & O_APPEND) logical_write_offset += written;
      } else {
          stats.last_error_code = (written < 0) ? errno : EIO;
      }

      // Return to pool
      free_write_pool.enqueue(req.data);

      if (write_worker_stop_flag) break;
    }
  }

  void readWorkerTask() {
    size_t scratch_capacity = read_chunk_size;
    auto temp_buffer = std::make_unique<char[]>(scratch_capacity);

    while (true) {
      std::unique_lock<std::mutex> lock(read_mutex);
      
      bool needs_fill = (read_buffer.available_space() > 0 && !read_eof_flag.load());
      if (!needs_fill || read_worker_stop_flag.load()) {
        read_task_running = false;
        break;
      }

      uint64_t my_gen = read_buffer_generation.load();
      off_t read_pos = read_head_in_storage.load();
      
      size_t n = std::min(read_chunk_size, read_buffer.available_space());
      if (scratch_capacity < n) {
          scratch_capacity = n;
          temp_buffer = std::make_unique<char[]>(scratch_capacity);
      }

      lock.unlock();
      auto start = std::chrono::steady_clock::now();
      ssize_t bytes_read = ops.pread(handle, temp_buffer.get(), n, read_pos);
      auto end = std::chrono::steady_clock::now();
      lock.lock();

      if (my_gen != read_buffer_generation.load()) {
          read_task_running = false;
          break;
      }

      if (bytes_read > 0) {
        read_buffer.write(temp_buffer.get(), bytes_read);
        read_head_in_storage += bytes_read;
        stats.bytes_read += bytes_read;
        stats.total_read_latency_us += std::chrono::duration_cast<std::chrono::microseconds>(end - start).count();
        stats.read_ops_count++;
      } else if (bytes_read == 0) {
        read_eof_flag = true;
      } else if (stats.last_error_code.load() == 0) {
        stats.last_error_code = errno;
      }
      
      read_cv_consumer.notify_all();

      if (read_buffer.available_space() == 0 || read_eof_flag.load() || read_worker_stop_flag.load()) {
          read_task_running = false;
          break;
      }
    }
  }
};

conveyor_t *conveyor_create(const conveyor_config_t *cfg) {
  if (!cfg) { errno = EINVAL; return nullptr; }
  auto *impl = new ConveyorImpl(cfg->initial_write_size, cfg->initial_read_size);
  impl->handle = cfg->handle;
  impl->flags = cfg->flags;
  impl->ops = cfg->ops;
  impl->max_write_capacity = (cfg->max_write_size > 0) ? cfg->max_write_size : cfg->initial_write_size;
  impl->max_read_capacity = (cfg->max_read_size > 0) ? cfg->max_read_size : cfg->initial_read_size;

  if (cfg->write_chunk_size > 0) impl->write_chunk_size = cfg->write_chunk_size;
  if (cfg->read_chunk_size > 0) impl->read_chunk_size = cfg->read_chunk_size;

  int mode = cfg->flags & O_ACCMODE;
  impl->read_buffer_enabled = (mode == O_RDONLY || mode == O_RDWR) && (cfg->initial_read_size > 0);
  impl->write_buffer_enabled = (mode == O_WRONLY || mode == O_RDWR) && (cfg->initial_write_size > 0);

  if (impl->read_buffer_enabled) impl->triggerReadTask();
  return reinterpret_cast<conveyor_t *>(impl);
}

void conveyor_destroy(conveyor_t *conv) {
  if (!conv) return;
  auto *impl = reinterpret_cast<ConveyorImpl *>(conv);
  conveyor_stop(conv);
  delete impl;
}

void conveyor_stop(conveyor_t *conv) {
  if (!conv) return;
  auto *impl = reinterpret_cast<ConveyorImpl *>(conv);
  
  if (impl->write_buffer_enabled) {
      impl->write_worker_stop_flag = true;
      while (impl->active_write_tasks > 0) std::this_thread::yield();
  }

  if (impl->read_buffer_enabled) {
      std::unique_lock<std::mutex> lock(impl->read_mutex);
      impl->read_worker_stop_flag = true;
      impl->read_cv_consumer.notify_all();
  }
}

ssize_t conveyor_write(conveyor_t *conv, const void* buf, size_t count) {
  if (!conv) { errno = EBADF; return LIBCONVEYOR_ERROR; }
  auto *impl = reinterpret_cast<ConveyorImpl *>(conv);
  if (impl->stats.last_error_code.load() != 0) { errno = impl->stats.last_error_code.load(); return LIBCONVEYOR_ERROR; }

  // Zero-Copy Submission Principle:
  // If count is large, we take a segment and hand it off.
  // Since the iRODS API gives us a const void*, we MUST copy once.
  // But we copy into a pool buffer without any global mutex.
  
  auto buf_ptr = impl->get_free_buffer();
  if (!buf_ptr) { errno = ENOMEM; return LIBCONVEYOR_ERROR; }

  // Copy-In (No central lock!)
  memcpy(buf_ptr.get(), buf, std::min(count, impl->write_chunk_size));
  size_t actual_len = std::min(count, impl->write_chunk_size);

  impl->write_queue.enqueue({impl->current_file_offset.fetch_add(actual_len), actual_len, buf_ptr});
  
  impl->triggerWriteTask();
  return actual_len;
}

ssize_t conveyor_read(conveyor_t *conv, void *buf, size_t count) {
  if (!conv) { errno = EBADF; return LIBCONVEYOR_ERROR; }
  auto *impl = reinterpret_cast<ConveyorImpl *>(conv);
  if (impl->stats.last_error_code.load() != 0) { errno = impl->stats.last_error_code.load(); return LIBCONVEYOR_ERROR; }

  char *ptr = static_cast<char *>(buf);
  off_t start_offset = impl->current_file_offset.load();
  ssize_t total_read = 0;
  {
    std::unique_lock<std::mutex> read_lock(impl->read_mutex);
    impl->last_read_end_offset = start_offset + count;
    while (total_read < count && !impl->read_worker_stop_flag.load()) {
      if (impl->read_buffer.empty()) {
        if (impl->read_eof_flag.load()) break;
        impl->triggerReadTask();
        impl->read_cv_consumer.wait(read_lock, [&] { return impl->read_buffer.available_data() > 0 || impl->read_worker_stop_flag.load(); });
        if (impl->read_buffer.available_data() == 0) break;
      }
      total_read += impl->read_buffer.read(ptr + total_read, count - total_read);
      impl->triggerReadTask();
    }
  }
  impl->current_file_offset += total_read;
  return total_read;
}

off_t conveyor_lseek(conveyor_t *conv, off_t offset, int whence) {
  if (!conv) { errno = EBADF; return LIBCONVEYOR_ERROR; }
  auto *impl = reinterpret_cast<ConveyorImpl *>(conv);
  std::unique_lock<std::mutex> lock(impl->read_mutex);
  off_t new_offset = (whence == SEEK_SET) ? offset : (whence == SEEK_CUR) ? impl->current_file_offset.load() + offset : impl->ops.lseek(impl->handle, offset, SEEK_END);
  if (new_offset == LIBCONVEYOR_ERROR) return LIBCONVEYOR_ERROR;
  if (new_offset != impl->current_file_offset.load()) {
    impl->current_file_offset = new_offset;
    impl->read_head_in_storage = new_offset;
    impl->read_buffer.clear();
    impl->read_eof_flag = false;
    impl->read_buffer_generation++;
    impl->triggerReadTask();
  }
  return new_offset;
}

int conveyor_flush(conveyor_t *conv) {
  if (!conv) { errno = EBADF; return LIBCONVEYOR_ERROR; }
  auto *impl = reinterpret_cast<ConveyorImpl *>(conv);
  if (!impl->write_buffer_enabled) return 0;
  
  // Busy wait for queue to drain
  while (impl->write_queue.size_approx() > 0) {
      impl->triggerWriteTask();
      std::this_thread::yield();
  }
  return (impl->stats.last_error_code.load() == 0) ? 0 : LIBCONVEYOR_ERROR;
}

int conveyor_get_stats(conveyor_t *conv, conveyor_stats_t *stats) {
  if (!conv || !stats) { errno = EINVAL; return LIBCONVEYOR_ERROR; }
  auto *impl = reinterpret_cast<ConveyorImpl *>(conv);
  stats->bytes_written = impl->stats.bytes_written.exchange(0);
  stats->bytes_read = impl->stats.bytes_read.exchange(0);
  size_t w_ops = impl->stats.write_ops_count.exchange(0);
  size_t r_ops = impl->stats.read_ops_count.exchange(0);
  stats->avg_write_latency_ms = (w_ops > 0) ? (impl->stats.total_write_latency_us.exchange(0) / (w_ops * 1000)) : 0;
  stats->avg_read_latency_ms = (r_ops > 0) ? (impl->stats.total_read_latency_us.exchange(0) / (r_ops * 1000)) : 0;
  stats->write_buffer_full_events = impl->stats.write_buffer_full_events.exchange(0);
  stats->last_error_code = impl->stats.last_error_code.load();
  return 0;
}

int conveyor_clear_error(conveyor_t* conv) {
    if (!conv) return -1;
    auto* impl = reinterpret_cast<ConveyorImpl*>(conv);
    impl->stats.last_error_code = 0;
    return 0;
}

} // namespace libconveyor
