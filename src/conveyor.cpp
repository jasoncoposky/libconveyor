#include <iostream>
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
#include <cstdint>
#include <unordered_map>


#ifndef O_ACCMODE
#define O_ACCMODE (O_RDONLY | O_WRONLY | O_RDWR)
#endif

namespace libconveyor {

struct Segment {
    void* data;
    off_t file_offset;
    std::atomic<size_t> cursor;
    std::atomic<int> ref_count;
    std::atomic<bool> is_full;
    std::atomic<bool> is_enqueued;
    size_t capacity;

    Segment(void* d, size_t cap) 
        : data(d), file_offset(0), cursor(0), ref_count(0), is_full(false), is_enqueued(false), capacity(cap) {}
    
    ~Segment() { if (data) free(data); }
};

typedef std::shared_ptr<Segment> SegmentPtr;

struct WriteRequest {
    SegmentPtr segment;
};

struct ConveyorImpl {
    storage_handle_t handle;
    int flags;
    storage_operations_t ops;
    size_t write_chunk_size = 32 * 1024 * 1024;
    size_t read_chunk_size = 32 * 1024 * 1024;
    size_t max_write_capacity = 0;
    std::atomic<size_t> current_write_capacity{0};

    moodycamel::ConcurrentQueue<SegmentPtr> free_segments;
    moodycamel::ConcurrentQueue<WriteRequest> write_queue;

    std::mutex rotation_mutex;
    SegmentPtr active_segment; // Guarded by rotation_mutex

    std::atomic<bool> write_worker_stop_flag{false};
    std::atomic<int> active_write_tasks{0};
    std::atomic<int> active_read_tasks{0};
    std::atomic<off_t> current_file_offset{0};
    std::atomic<off_t> logical_append_pos{0};

    std::shared_ptr<ThreadPool> thread_pool;

    RingBuffer read_buffer;
    std::mutex read_mutex;
    std::condition_variable read_cv_consumer;
    std::atomic<bool> read_task_running{false};
    std::atomic<bool> read_worker_stop_flag{false};
    std::atomic<bool> read_eof_flag{false};
    std::atomic<off_t> read_head_in_storage{0};

    std::unordered_map<void*, SegmentPtr> active_external_segments;
    std::mutex external_segments_mutex;

    struct {
        std::atomic<size_t> bytes_written{0};
        std::atomic<size_t> bytes_read{0};
        std::atomic<int> last_error_code{0};
    } stats;

    ConveyorImpl(size_t w_cap, size_t r_cap, size_t w_chunk)
        : read_buffer(r_cap ? r_cap : 1), write_chunk_size(w_chunk), max_write_capacity(w_cap) {
        
        thread_pool = ThreadPool::get_shared_instance();

        if (w_cap > 0) {
            size_t num_segments = std::min((size_t)256, std::max((size_t)1, w_cap / w_chunk));
            for (size_t i = 0; i < num_segments; ++i) {
                allocate_and_enqueue_segment();
            }
        }
    }

    ~ConveyorImpl() {
        write_worker_stop_flag = true;
        read_worker_stop_flag = true;
        read_cv_consumer.notify_all();
        
        // Ensure flushes and prefetchers are done
        while (active_write_tasks.load() > 0 || active_read_tasks.load() > 0) {
            std::this_thread::yield();
        }
    }

    bool allocate_and_enqueue_segment() {
        if (current_write_capacity.load() >= max_write_capacity && max_write_capacity > 0) return false;
        
        void* raw_ptr = nullptr;
        if (posix_memalign(&raw_ptr, 4096, write_chunk_size) == 0) {
            free_segments.enqueue(std::make_shared<Segment>(raw_ptr, write_chunk_size));
            current_write_capacity.fetch_add(write_chunk_size);
            return true;
        }
        return false;
    }

    void triggerWriteTask() {
        if (write_queue.size_approx() == 0) return;
        if (active_write_tasks.load(std::memory_order_relaxed) > 32) return;
        active_write_tasks.fetch_add(1);
        thread_pool->submit_detached([this]() {
            this->writeWorkerTask();
            active_write_tasks.fetch_sub(1);
        });
    }

    void writeWorkerTask() {
        while (true) {
            WriteRequest req;
            if (!write_queue.try_dequeue(req)) {
                if (write_worker_stop_flag.load()) break;
                std::this_thread::sleep_for(std::chrono::microseconds(100));
                if (!write_queue.size_approx()) break; 
                continue;
            }
            SegmentPtr seg = req.segment;
            if (!seg || !seg->data) continue;

            size_t bytes_to_write = seg->cursor.load();
            size_t bytes_written = 0;
            bool error_occurred = false;

            while (bytes_written < bytes_to_write) {
                ssize_t ret = ops.pwrite(handle, 
                                         (char*)seg->data + bytes_written, 
                                         bytes_to_write - bytes_written, 
                                         seg->file_offset + bytes_written);
                if (ret > 0) {
                    bytes_written += ret;
                } else if (ret == 0) {
                    stats.last_error_code = EIO;
                    error_occurred = true;
                    break;
                } else {
                    if (errno == EINTR) continue;
                    stats.last_error_code = errno;
                    error_occurred = true;
                    break;
                }
            }

            if (!error_occurred) {
                stats.bytes_written += (size_t)bytes_written;
            }

            seg->cursor = 0;
            seg->is_full = false;
            seg->is_enqueued = false;
            seg->ref_count = 0;
            free_segments.enqueue(seg);
        }
    }

    void readWorkerTask() {
        active_read_tasks.fetch_add(1);
        if (read_buffer.capacity <= 1) {
            read_task_running = false;
            active_read_tasks.fetch_sub(1);
            return;
        }
        auto temp_buffer = std::make_unique<char[]>(read_chunk_size);
        while (true) {
            std::unique_lock<std::mutex> lock(read_mutex);
            if (read_buffer.available_space() == 0 || read_eof_flag.load() || read_worker_stop_flag.load()) {
                read_task_running = false; 
                break;
            }
            off_t read_pos = read_head_in_storage.load();
            size_t n = std::min(read_chunk_size, read_buffer.available_space());
            lock.unlock();
            ssize_t bytes_read = ops.pread(handle, temp_buffer.get(), n, read_pos);
            lock.lock();
            if (bytes_read > 0) {
                read_buffer.write(temp_buffer.get(), (size_t)bytes_read);
                read_head_in_storage += (off_t)bytes_read;
                stats.bytes_read += (size_t)bytes_read;
            } else if (bytes_read == 0) { read_eof_flag = true; }
            read_cv_consumer.notify_all();
        }
        active_read_tasks.fetch_sub(1);
    }
};

} // namespace libconveyor

using namespace libconveyor;

extern "C" {

conveyor_t *conveyor_create(const conveyor_config_t *cfg) {
    if (!cfg) return nullptr;
    size_t w_chunk = (cfg->write_chunk_size > 0) ? cfg->write_chunk_size : 32 * 1024 * 1024;
    auto *impl = new ConveyorImpl(cfg->initial_write_size, cfg->initial_read_size, w_chunk);
    impl->handle = cfg->handle;
    impl->flags = cfg->flags;
    impl->ops = cfg->ops;

    if (impl->flags & O_APPEND) {
        impl->logical_append_pos = impl->ops.lseek(impl->handle, 0, SEEK_END);
        impl->current_file_offset = impl->logical_append_pos.load();
    } else {
        impl->current_file_offset = impl->ops.lseek(impl->handle, 0, SEEK_CUR);
    }
    impl->read_head_in_storage = impl->current_file_offset.load();
    return reinterpret_cast<conveyor_t *>(impl);
}

void conveyor_destroy(conveyor_t *conv) {
    if (!conv) return;
    auto *impl = reinterpret_cast<ConveyorImpl *>(conv);
    conveyor_flush(conv);
    delete impl;
}

void conveyor_stop(conveyor_t *conv) {}

ssize_t conveyor_write(conveyor_t *conv, const void* buf, size_t count) {
    auto *impl = reinterpret_cast<ConveyorImpl *>(conv);
    if ((impl->flags & O_ACCMODE) == O_RDONLY) {
        errno = EBADF;
        return LIBCONVEYOR_ERROR;
    }
    if (!buf || count == 0) return 0;
    if (count > impl->write_chunk_size) {
        errno = EMSGSIZE;
        return LIBCONVEYOR_ERROR;
    }
    const char* in_ptr = static_cast<const char*>(buf);
    size_t total_written = 0;

    while (total_written < count) {
        SegmentPtr seg;
        {
            std::lock_guard<std::mutex> lock(impl->rotation_mutex);
            seg = impl->active_segment;
            if (!seg || seg->is_full.load(std::memory_order_relaxed)) {
                if (seg) {
                    seg->is_full = true;
                    if (seg->ref_count.load() == 0 && !seg->is_enqueued.exchange(true)) {
                        impl->write_queue.enqueue({seg});
                        impl->triggerWriteTask();
                    }
                }
                if (!impl->free_segments.try_dequeue(seg)) {
                    if (!impl->allocate_and_enqueue_segment() || !impl->free_segments.try_dequeue(seg)) {
                        break; 
                    }
                }
                seg->file_offset = (impl->flags & O_APPEND) ? impl->logical_append_pos.load() : impl->current_file_offset.load();
                seg->cursor = 0;
                seg->ref_count = 0;
                seg->is_full = false;
                seg->is_enqueued = false;
                impl->active_segment = seg;
            }
        }

        size_t remaining = count - total_written;
        size_t current_cursor = seg->cursor.load(std::memory_order_relaxed);
        size_t reservation = std::min(remaining, (seg->capacity > current_cursor) ? (seg->capacity - current_cursor) : 0);

        if (reservation == 0) {
            seg->is_full = true;
            continue;
        }

        size_t offset_in_seg = seg->cursor.fetch_add(reservation);
        if (offset_in_seg + reservation > seg->capacity) {
            seg->is_full = true;
            continue;
        }

        seg->ref_count.fetch_add(1);
        std::memcpy((char*)seg->data + offset_in_seg, in_ptr + total_written, reservation);
        total_written += reservation;
        
        impl->current_file_offset.fetch_add((off_t)reservation);
        if (impl->flags & O_APPEND) impl->logical_append_pos.fetch_add((off_t)reservation);

        if (seg->ref_count.fetch_sub(1) == 1 && seg->is_full.load()) {
            if (!seg->is_enqueued.exchange(true)) {
                impl->write_queue.enqueue({seg});
                impl->triggerWriteTask();
            }
        }
    }
    return (ssize_t)total_written;
}

ssize_t conveyor_read(conveyor_t *conv, void *buf, size_t count) {
    auto *impl = reinterpret_cast<ConveyorImpl *>(conv);
    if ((impl->flags & O_ACCMODE) == O_WRONLY) {
        errno = EBADF;
        return LIBCONVEYOR_ERROR;
    }
    if (impl->read_buffer.capacity <= 1) {
        errno = EINVAL;
        return LIBCONVEYOR_ERROR;
    }
    char *ptr = static_cast<char *>(buf);
    ssize_t total_read = 0;
    
    off_t read_start_offset = impl->current_file_offset.load();

    std::unique_lock<std::mutex> read_lock(impl->read_mutex);
    while (total_read < (ssize_t)count && !impl->read_worker_stop_flag.load()) {
        if (impl->read_buffer.empty()) {
            if (impl->read_eof_flag.load()) break;
            if (!impl->read_task_running.exchange(true)) {
                impl->thread_pool->submit_detached([impl]() { impl->readWorkerTask(); });
            }
            impl->read_cv_consumer.wait(read_lock, [&] { 
                return impl->read_buffer.available_data() > 0 || impl->read_worker_stop_flag.load() || impl->read_eof_flag.load(); 
            });
            if (impl->read_buffer.empty()) break;
        }
        total_read += (ssize_t)impl->read_buffer.read(ptr + total_read, count - (size_t)total_read);
    }
    read_lock.unlock();

    SegmentPtr active;
    {
        std::lock_guard<std::mutex> lock(impl->rotation_mutex);
        active = impl->active_segment;
    }
    
    if (active) {
        off_t seg_start = active->file_offset;
        size_t seg_len = active->cursor.load(std::memory_order_relaxed);
        
        off_t overlap_start = std::max(read_start_offset, seg_start);
        off_t overlap_end = std::min(read_start_offset + (off_t)count, seg_start + (off_t)seg_len);
        
        if (overlap_start < overlap_end) {
            size_t dest_offset = (size_t)(overlap_start - read_start_offset);
            size_t src_offset = (size_t)(overlap_start - seg_start);
            size_t copy_len = (size_t)(overlap_end - overlap_start);
            std::memcpy(ptr + dest_offset, (char*)active->data + src_offset, copy_len);
            total_read = std::max((ssize_t)(dest_offset + copy_len), total_read);
        }
    }

    return total_read;
}

off_t conveyor_lseek(conveyor_t *conv, off_t offset, int whence) {
    auto *impl = reinterpret_cast<ConveyorImpl *>(conv);
    impl->current_file_offset = offset;
    return offset;
}

int conveyor_flush(conveyor_t *conv) {
    auto *impl = reinterpret_cast<ConveyorImpl *>(conv);
    {
        std::lock_guard<std::mutex> lock(impl->rotation_mutex);
        SegmentPtr seg = impl->active_segment;
        if (seg && seg->cursor.load() > 0) {
            seg->is_full = true;
            impl->active_segment = nullptr;
            if (seg->ref_count.load() == 0 && !seg->is_enqueued.exchange(true)) {
                impl->write_queue.enqueue({seg});
                impl->triggerWriteTask();
            }
        }
    }
    while (impl->write_queue.size_approx() > 0 || impl->active_write_tasks.load() > 0) {
        impl->triggerWriteTask();
        std::this_thread::yield();
    }
    return (impl->stats.last_error_code.load() != 0) ? LIBCONVEYOR_ERROR : 0;
}

int conveyor_get_stats(conveyor_t *conv, conveyor_stats_t *stats) {
    if (!conv || !stats) return -1;
    auto *impl = reinterpret_cast<ConveyorImpl *>(conv);
    stats->bytes_written = impl->stats.bytes_written.exchange(0);
    stats->bytes_read = impl->stats.bytes_read.exchange(0);
    stats->last_error_code = impl->stats.last_error_code.exchange(0);
    stats->avg_write_latency_ms = 0;
    stats->avg_read_latency_ms = 0;
    stats->write_buffer_full_events = 0;
    return 0;
}

int conveyor_clear_error(conveyor_t* conv) {
    if (!conv) return -1;
    auto *impl = reinterpret_cast<ConveyorImpl *>(conv);
    impl->stats.last_error_code = 0;
    return 0;
}

void* conveyor_get_buffer(conveyor_t* conv, size_t* size) {
    auto *impl = reinterpret_cast<ConveyorImpl *>(conv);
    SegmentPtr seg;
    if (impl->free_segments.try_dequeue(seg)) {
        if (size) *size = seg->capacity;
        seg->cursor = 0;
        seg->ref_count = 1; 
        seg->is_full = false;
        
        {
            std::lock_guard<std::mutex> lock(impl->external_segments_mutex);
            impl->active_external_segments[seg->data] = seg;
        }
        
        return seg->data; 
    }
    return nullptr;
}

ssize_t conveyor_submit_buffer(conveyor_t* conv, void* buf, size_t size, off_t offset) {
    auto *impl = reinterpret_cast<ConveyorImpl *>(conv);
    SegmentPtr seg;
    
    {
        std::lock_guard<std::mutex> lock(impl->external_segments_mutex);
        auto it = impl->active_external_segments.find(buf);
        if (it != impl->active_external_segments.end()) {
            seg = it->second;
            impl->active_external_segments.erase(it);
        }
    }
    
    if (!seg) {
        seg = std::make_shared<Segment>(buf, size);
    }
    
    seg->cursor = size;
    seg->file_offset = offset;
    seg->is_full = true;
    seg->is_enqueued = true;
    impl->write_queue.enqueue({seg});
    impl->triggerWriteTask();
    return (ssize_t)size;
}

void conveyor_release_buffer(conveyor_t* conv, void* buf) {
    auto *impl = reinterpret_cast<ConveyorImpl *>(conv);
    SegmentPtr seg;
    {
        std::lock_guard<std::mutex> lock(impl->external_segments_mutex);
        auto it = impl->active_external_segments.find(buf);
        if (it != impl->active_external_segments.end()) {
            seg = it->second;
            impl->active_external_segments.erase(it);
        }
    }
    if (seg) {
        seg->ref_count = 0;
        impl->free_segments.enqueue(seg);
    }
}

} // extern "C"
