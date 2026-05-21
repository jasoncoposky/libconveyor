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
  size_t ring_buffer_pos;
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
  RingBuffer write_ring_buffer;
  moodycamel::ConcurrentQueue<WriteRequest> write_queue;

  std::mutex write_mutex; // Protects write_ring_buffer and serializes storage I/O
  std::condition_variable write_cv_producer;
  std::atomic<bool> write_task_running{false};
  std::atomic<bool> write_worker_stop_flag{false};
  std::atomic<bool> write_buffer_needs_flush{false};

  // Read Logic
  bool read_buffer_enabled = false;
  RingBuffer read_buffer;
  std::mutex read_mutex; // Protects read_buffer and serializes storage I/O
  std::condition_variable read_cv_consumer;
  std::atomic<bool> read_task_running{false};
  std::atomic<bool> read_worker_stop_flag{false};
  std::atomic<bool> read_worker_needs_fill{false};
  std::atomic<bool> read_eof_flag{false};

  std::atomic<uint64_t> read_buffer_generation{0};

  std::atomic<off_t> logical_write_offset{0};
  std::atomic<off_t> read_head_in_storage{0};
  std::atomic<off_t> current_file_offset{0};

  off_t last_read_end_offset = 0;
  size_t sequential_read_counter = 0;

  struct Stats {
    std::atomic<size_t> bytes_written{0};
    std::atomic<size_t> bytes_read{0};
    std::atomic<size_t> total_write_latency_us{0};
    std::atomic<size_t> write_ops_count{0};
    std::atomic<size_t> total_read_latency_us{0};
    std::atomic<size_t> read_ops_count{0};
    std::atomic<size_t> write_buffer_full_events{0};
    std::atomic<int> last_error_code{0};
  } stats;

  ConveyorImpl(size_t w_cap, size_t r_cap)
      : write_ring_buffer(w_cap), read_buffer(r_cap), max_write_capacity(w_cap),
        max_read_capacity(r_cap) {}

  void triggerWriteTask() {
      if (write_worker_stop_flag.load(std::memory_order_relaxed)) return;
      if (write_task_running.load(std::memory_order_relaxed)) return;
      if (write_task_running.exchange(true)) return;

      ThreadPool::instance().submit_detached([this]() {
          this->writeWorkerTask();
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
    std::vector<char> scratch_buffer;
    scratch_buffer.reserve(write_chunk_size); 

    while (true) {
      std::vector<WriteRequest> coalesced_reqs;
      WriteRequest req;
      
      // --- COALESCING ENGINE ---
      while (true) {
          if (write_queue.try_dequeue(req)) {
              if (coalesced_reqs.empty()) {
                  coalesced_reqs.push_back(req);
              } else {
                  const auto& last = coalesced_reqs.back();
                  bool offset_match = (req.file_offset == (off_t)(last.file_offset + last.length));
                  bool ring_linear = (req.ring_buffer_pos == (last.ring_buffer_pos + last.length));
                  
                  if (offset_match && ring_linear && (coalesced_reqs.size() < 1024)) {
                      coalesced_reqs.back().length += req.length;
                  } else {
                      coalesced_reqs.push_back(req);
                      break; 
                  }
              }
              if (coalesced_reqs.back().length >= write_chunk_size) break;
          } else {
              // Queue empty. If we have very little work, spin briefly to see if more arrives.
              // This amortizes high backend latency.
              if (!coalesced_reqs.empty() && coalesced_reqs.back().length < (write_chunk_size / 2)) {
                  bool found = false;
                  for (int spin = 0; spin < 5000; ++spin) {
                      if (write_queue.try_dequeue(req)) {
                          found = true; break;
                      }
                      std::this_thread::yield();
                  }
                  if (found) continue; // Found more work, continue coalescing
              }
              break; // No more work after spin, or already have enough
          }
      }

      if (coalesced_reqs.empty()) {
          std::unique_lock<std::mutex> lock(write_mutex);
          write_buffer_needs_flush = false;
          write_cv_producer.notify_all();
          
          write_task_running = false;
          if (write_queue.size_approx() > 0) {
              if (!write_task_running.exchange(true)) continue;
          }
          break;
      }

      for (const auto& chunk : coalesced_reqs) {
          if (write_worker_stop_flag) break;

          if (scratch_buffer.capacity() < chunk.length) scratch_buffer.reserve(chunk.length);
          scratch_buffer.resize(chunk.length);

          {
              std::lock_guard<std::mutex> lock(write_mutex);
              write_ring_buffer.peek_at(chunk.ring_buffer_pos, scratch_buffer.data(), chunk.length);
          }

          off_t write_pos = (flags & O_APPEND) ? logical_write_offset.load() : chunk.file_offset;
          auto start = std::chrono::steady_clock::now();
          
          ssize_t written_now = ops.pwrite(handle, scratch_buffer.data(), chunk.length, write_pos);
          bool write_error = (written_now < (ssize_t)chunk.length);
          
          auto end = std::chrono::steady_clock::now();

          {
              std::lock_guard<std::mutex> lock(write_mutex);
              write_ring_buffer.read(nullptr, chunk.length);
              write_cv_producer.notify_all();
          }

          if (!write_error) {
            stats.bytes_written += chunk.length;
            stats.total_write_latency_us += std::chrono::duration_cast<std::chrono::microseconds>(end - start).count();
            stats.write_ops_count++;
            if (flags & O_APPEND) logical_write_offset += chunk.length;
          } else if (stats.last_error_code.load() == 0) {
            stats.last_error_code = (written_now < 0) ? errno : EIO;
          }
      }

      if (write_worker_stop_flag) {
        write_task_running = false;
        break;
      }
    }
  }

  void readWorkerTask() {
    std::vector<char> temp_buffer;
    temp_buffer.reserve(read_chunk_size); 

    while (true) {
      std::unique_lock<std::mutex> lock(read_mutex);
      
      bool needs_fill = (read_buffer.available_space() > 0 && !read_eof_flag.load());
      if (!needs_fill || read_worker_stop_flag.load()) {
        read_task_running = false;
        if (!read_worker_stop_flag.load()) {
            if (read_buffer.available_space() > 0 && !read_eof_flag.load()) {
                if (!read_task_running.exchange(true)) continue;
            }
        }
        break;
      }

      uint64_t my_gen = read_buffer_generation.load();
      off_t read_pos = read_head_in_storage.load();
      
      // BULK PREFETCH: Fetch as much as we can fit (up to chunk size)
      size_t n = std::min(read_chunk_size, read_buffer.available_space());
      if (temp_buffer.capacity() < n) temp_buffer.reserve(n);
      temp_buffer.resize(n);

      lock.unlock();
      auto start = std::chrono::steady_clock::now();
      ssize_t bytes_read = ops.pread(handle, temp_buffer.data(), n, read_pos);
      auto end = std::chrono::steady_clock::now();
      lock.lock();

      if (my_gen != read_buffer_generation.load()) {
          read_task_running = false;
          break;
      }

      if (bytes_read > 0) {
        read_buffer.write(temp_buffer.data(), bytes_read);
        read_head_in_storage += bytes_read;
        stats.bytes_read += bytes_read;
        stats.total_read_latency_us += std::chrono::duration_cast<std::chrono::microseconds>(end - start).count();
        stats.read_ops_count++;
      } else if (bytes_read == 0) {
        read_eof_flag = true;
      } else if (stats.last_error_code.load() == 0) {
        stats.last_error_code = errno;
      }
      
      if (read_worker_needs_fill.load()) read_worker_needs_fill = false;
      read_cv_consumer.notify_all();

      if (read_buffer.available_space() == 0 || read_eof_flag.load() || read_worker_stop_flag.load()) {
          read_task_running = false;
          break;
      }
    }
  }

  bool is_idle() const {
      return write_queue.size_approx() == 0;
  }
};
} // namespace libconveyor

using namespace libconveyor;

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
  if (impl->write_buffer_enabled) {
    if (impl->flags & O_APPEND) {
      off_t sz = impl->ops.lseek(impl->handle, 0, SEEK_END);
      if (sz == LIBCONVEYOR_ERROR) { delete impl; return nullptr; }
      impl->logical_write_offset = sz;
      impl->current_file_offset = sz;
      impl->read_head_in_storage = sz;
    }
  }
  return reinterpret_cast<conveyor_t *>(impl);
}

void conveyor_destroy(conveyor_t *conv) {
  if (!conv) return;
  auto *impl = reinterpret_cast<ConveyorImpl *>(conv);
  if (impl->write_buffer_enabled) conveyor_flush(conv);
  
  impl->read_worker_stop_flag = true;
  impl->write_worker_stop_flag = true;

  while (impl->read_task_running.load() || impl->write_task_running.load()) {
      std::this_thread::yield();
  }

  delete impl;
}

ssize_t conveyor_write(conveyor_t *conv, const void *buf, size_t count) {
  if (!conv) { errno = EBADF; return LIBCONVEYOR_ERROR; }
  auto *impl = reinterpret_cast<ConveyorImpl *>(conv);
  int mode = impl->flags & O_ACCMODE;
  if (mode != O_WRONLY && mode != O_RDWR) { errno = EBADF; return LIBCONVEYOR_ERROR; }
  if (count > impl->max_write_capacity) { errno = EMSGSIZE; return LIBCONVEYOR_ERROR; }
  if (!impl->write_buffer_enabled) return impl->ops.pwrite(impl->handle, buf, count, impl->current_file_offset.load());
  if (impl->stats.last_error_code.load() != 0) { errno = impl->stats.last_error_code.load(); return LIBCONVEYOR_ERROR; }

  std::unique_lock<std::mutex> lock(impl->write_mutex);
  if (impl->write_ring_buffer.available_space() < count) {
    if (impl->write_ring_buffer.capacity < impl->max_write_capacity) {
      if (!impl->is_idle()) { // Check if queue is not empty
        impl->write_buffer_needs_flush = true;
        impl->triggerWriteTask();
        impl->write_cv_producer.wait(lock, [&] { return impl->is_idle() || impl->write_worker_stop_flag; });
      }
      impl->write_buffer_needs_flush = false;
      size_t needed = impl->write_ring_buffer.size + count;
      size_t new_cap = std::min(impl->max_write_capacity, std::max(needed, impl->write_ring_buffer.capacity * 2));
      if (new_cap >= needed) impl->write_ring_buffer.resize(new_cap);
    }
  }
  if (!impl->write_cv_producer.wait_for(lock, std::chrono::seconds(30), [&] {
        return (impl->write_ring_buffer.available_space() >= count) || impl->write_worker_stop_flag;
      })) { errno = ETIMEDOUT; return LIBCONVEYOR_ERROR; }

  if (impl->write_worker_stop_flag) return LIBCONVEYOR_ERROR;
  size_t ring_pos_start = impl->write_ring_buffer.head;
  impl->write_ring_buffer.write(static_cast<const char *>(buf), count);
  impl->write_queue.enqueue({impl->current_file_offset.load(), count, ring_pos_start});
  impl->current_file_offset += count;
  
  impl->triggerWriteTask();
  return count;
}

ssize_t conveyor_read(conveyor_t *conv, void *buf, size_t count) {
  if (!conv) { errno = EBADF; return LIBCONVEYOR_ERROR; }
  auto *impl = reinterpret_cast<ConveyorImpl *>(conv);
  if (!impl->read_buffer_enabled) { errno = EBADF; return LIBCONVEYOR_ERROR; }
  if (impl->stats.last_error_code.load() != 0) { errno = impl->stats.last_error_code.load(); return LIBCONVEYOR_ERROR; }

  char *ptr = static_cast<char *>(buf);
  off_t start_offset = impl->current_file_offset.load();
  ssize_t total_read = 0;
  {
    std::unique_lock<std::mutex> read_lock(impl->read_mutex);
    bool grow = (count > impl->read_buffer.capacity);
    if (impl->read_buffer.empty() && start_offset == impl->last_read_end_offset) {
      if (++impl->sequential_read_counter > 2) grow = true;
    } else { impl->sequential_read_counter = 0; }
    if (grow && impl->read_buffer.capacity < impl->max_read_capacity) {
      size_t new_cap = std::min(impl->max_read_capacity, std::max(count, impl->read_buffer.capacity * 2));
      impl->read_buffer.resize(new_cap);
    }
    impl->last_read_end_offset = start_offset + count;
    while (total_read < count && !impl->read_worker_stop_flag.load()) {
      if (impl->read_buffer.empty()) {
        if (impl->read_eof_flag.load()) break;
        impl->read_worker_needs_fill = true;
        impl->triggerReadTask();
        impl->read_cv_consumer.wait(read_lock, [&] { return impl->read_buffer.available_data() > 0 || impl->read_worker_stop_flag.load(); });
        if (impl->read_buffer.available_data() == 0) break;
      }
      total_read += impl->read_buffer.read(ptr + total_read, count - total_read);
      impl->triggerReadTask();
    }
  }
  if (impl->write_buffer_enabled) {
      // Snooping skipped for now
  }
  impl->current_file_offset = start_offset + total_read;
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
    impl->read_buffer_generation++;
    impl->read_eof_flag = false;
    impl->read_worker_needs_fill = true;
    impl->triggerReadTask();
  }
  return impl->current_file_offset.load();
}

int conveyor_flush(conveyor_t *conv) {
  if (!conv) { errno = EBADF; return -1; }
  auto *impl = reinterpret_cast<ConveyorImpl *>(conv);
  if (!impl->write_buffer_enabled) return 0;
  std::unique_lock<std::mutex> lock(impl->write_mutex);
  impl->write_buffer_needs_flush = true;
  impl->triggerWriteTask();
  impl->write_cv_producer.wait(lock, [&] { return impl->is_idle() || impl->write_worker_stop_flag; });
  return (impl->stats.last_error_code.load() == 0) ? 0 : -1;
}

int conveyor_is_idle(conveyor_t *conv) {
  if (!conv) return 1;
  auto *impl = reinterpret_cast<ConveyorImpl *>(conv);
  return impl->is_idle() ? 1 : 0;
}

int conveyor_get_stats(conveyor_t* conv, conveyor_stats_t* stats) {
  if (!conv || !stats) return -1;
  auto* impl = reinterpret_cast<ConveyorImpl*>(conv);
  stats->bytes_written = impl->stats.bytes_written.exchange(0);
  stats->bytes_read = impl->stats.bytes_read.exchange(0);
  size_t w_ops = impl->stats.write_ops_count.exchange(0), w_lat = impl->stats.total_write_latency_us.exchange(0);
  stats->avg_write_latency_ms = (w_ops > 0) ? (w_lat / w_ops) : 0;
  size_t r_ops = impl->stats.read_ops_count.exchange(0), r_lat = impl->stats.total_read_latency_us.exchange(0);
  stats->avg_read_latency_ms = (r_ops > 0) ? (r_lat / r_ops) : 0;
  stats->write_buffer_full_events = impl->stats.write_buffer_full_events.exchange(0);
  stats->last_error_code = impl->stats.last_error_code.exchange(0);
  return 0;
}

void conveyor_stop(conveyor_t* conv) {
    if (!conv) return;
    auto* impl = reinterpret_cast<ConveyorImpl*>(conv);
    impl->read_worker_stop_flag = true;
    impl->write_worker_stop_flag = true;
}

int conveyor_clear_error(conveyor_t* conv) {
    if (!conv) return -1;
    auto* impl = reinterpret_cast<ConveyorImpl*>(conv);
    impl->stats.last_error_code.store(0);
    return 0;
}
