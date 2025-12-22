#include "libconveyor/conveyor.h"
#include "libconveyor/detail/ring_buffer.h"
#include <vector>
#include <deque>
#include <mutex>
#include <condition_variable>
#include <thread>
#include <atomic>
#include <cerrno>
#include <cstring> // For memcpy
#include <algorithm>
#include <chrono>

#ifndef O_ACCMODE
#define O_ACCMODE (O_RDONLY | O_WRONLY | O_RDWR)
#endif

namespace libconveyor {

// --- OPTIMIZATION 1: Lightweight Metadata Struct ---
// No longer owns a std::vector. Just points to the RingBuffer.
struct WriteRequest {
    off_t file_offset;      // Where in the file to write
    size_t length;          // Length of data
    size_t ring_buffer_pos; // Starting index in the write_ring_buffer
};

struct ConveyorImpl {
    storage_handle_t handle;
    int flags;
    storage_operations_t ops;

    // --- ADAPTIVE CONFIG ---
    size_t max_write_capacity = 0;
    size_t max_read_capacity = 0;

    // Write Logic
    bool write_buffer_enabled = false;
    RingBuffer write_ring_buffer; // <--- The Write Buffer
    std::deque<WriteRequest> write_queue; // Queue of metadata only
    
    std::thread write_worker_thread;
    std::mutex write_mutex;
    std::condition_variable write_cv_producer;
    std::condition_variable write_cv_consumer;
    std::atomic<bool> write_worker_stop_flag{false};
    std::atomic<bool> write_buffer_needs_flush{false};
    
    // Read Logic
    bool read_buffer_enabled = false;
    RingBuffer read_buffer;
    std::thread read_worker_thread;
    std::mutex read_mutex;
    std::condition_variable read_cv_producer;
    std::condition_variable read_cv_consumer;
    std::atomic<bool> read_worker_stop_flag{false};
    std::atomic<bool> read_worker_needs_fill{false};
    std::atomic<bool> read_eof_flag{false};
    
    std::atomic<uint64_t> read_buffer_generation{0}; 

    std::atomic<off_t> logical_write_offset{0};
    std::atomic<off_t> read_head_in_storage{0};
    std::atomic<off_t> current_file_offset{0};

    // New Tracking for Read Heuristics
    off_t last_read_end_offset = 0;
    size_t sequential_read_counter = 0;

    struct Stats {
        std::atomic<size_t> bytes_written{0};
        std::atomic<size_t> bytes_read{0};
        std::atomic<size_t> total_write_latency_ms{0};
        std::atomic<size_t> write_ops_count{0};
        std::atomic<size_t> total_read_latency_ms{0};
        std::atomic<size_t> read_ops_count{0};
        std::atomic<size_t> write_buffer_full_events{0};
        std::atomic<int> last_error_code{0};
    } stats;
    
    ConveyorImpl(size_t w_cap, size_t r_cap) 
        : write_ring_buffer(w_cap), read_buffer(r_cap),
          max_write_capacity(w_cap), max_read_capacity(r_cap) {} // Default max = initial

    void writeWorker() {
        // Optimization: Reusable scratch buffer to handle ring-wrap-around
        // avoids re-allocating memory inside the loop.
        std::vector<char> scratch_buffer;
        scratch_buffer.reserve(4096); 

        while (true) {
            std::unique_lock<std::mutex> lock(write_mutex);
            write_cv_consumer.wait(lock, [&] { 
                return !write_queue.empty() || write_buffer_needs_flush || write_worker_stop_flag; 
            });
            if (write_worker_stop_flag && write_queue.empty()) break;

            if (write_queue.empty()) { 
                write_buffer_needs_flush = false;
                write_cv_producer.notify_all(); 
                continue;
            }

            // Grab the next request metadata
            WriteRequest req = write_queue.front();
            write_queue.pop_front();
            
            // --- CRITICAL SECTION: Copy data out of RingBuffer ---
            if (scratch_buffer.capacity() < req.length) {
                scratch_buffer.reserve(req.length);
            }
            scratch_buffer.resize(req.length);
            
            // Peek at the data from the ring buffer into our scratch space.
            // We don't advance the tail yet.
            write_ring_buffer.peek_at(req.ring_buffer_pos, scratch_buffer.data(), req.length);
            
            // --- IO SECTION STARTS ---
            lock.unlock(); 

            off_t write_pos;
            if (flags & O_APPEND) {
                write_pos = logical_write_offset.load(); 
            } else {
                write_pos = req.file_offset;
            }
            
            auto start = std::chrono::steady_clock::now();
            size_t total_written = 0;
            bool write_error = false;
            while (total_written < req.length) {
                ssize_t written_now = ops.pwrite(handle, scratch_buffer.data() + total_written, req.length - total_written, write_pos + total_written);
                if (written_now < 0) {
                    if (stats.last_error_code.load() == 0) {
                        stats.last_error_code = errno;
                    }
                    write_error = true;
                    break;
                }
                if (written_now == 0) {
                    if (stats.last_error_code.load() == 0) {
                        stats.last_error_code = EIO; 
                    }
                    write_error = true;
                    break;
                }
                total_written += written_now;
            }
            auto end = std::chrono::steady_clock::now();
            
            // --- IO SECTION ENDS ---
            lock.lock(); 
            
            // Now that IO is complete, we can advance the tail of the ring buffer
            // by "reading" into a null buffer.
            write_ring_buffer.read(nullptr, req.length);
            
            // Notify producers that space is now officially free.
            write_cv_producer.notify_all();
            
            if (!write_error) {
                stats.total_write_latency_ms += std::chrono::duration_cast<std::chrono::milliseconds>(end - start).count();
                stats.write_ops_count++;
                
                if (flags & O_APPEND) { 
                    logical_write_offset += total_written;
                }
            }
        }
    }

    void readWorker() {
        std::vector<char> temp_buffer;
        while (true) {
            std::unique_lock<std::mutex> lock(read_mutex);
            read_cv_producer.wait(lock, [&] { 
                return read_buffer.available_space() > 0 || read_worker_stop_flag.load() || read_worker_needs_fill.load(); 
            });
            if (read_worker_stop_flag.load()) break;

            if (read_buffer.available_space() > 0 && !read_worker_stop_flag.load()) {
                uint64_t my_gen = read_buffer_generation.load();
                
                off_t read_pos = current_file_offset + read_buffer.available_data();
                
                // --- CRITICAL: Read full available space (which might have grown!) ---
                size_t n = read_buffer.available_space();
                if (temp_buffer.capacity() < n) temp_buffer.reserve(n); // Grow scratch if needed
                temp_buffer.resize(n);
                
                lock.unlock(); 

                auto start = std::chrono::steady_clock::now();
                ssize_t bytes_read = ops.pread(handle, temp_buffer.data(), n, read_pos);
                auto end = std::chrono::steady_clock::now();
                
                lock.lock(); 

                if (my_gen != read_buffer_generation.load()) {
                    continue;
                }

                stats.total_read_latency_ms += std::chrono::duration_cast<std::chrono::milliseconds>(end - start).count();
                stats.read_ops_count++;

                if (bytes_read > 0) {
                    read_buffer.write(temp_buffer.data(), bytes_read);
                } else if (bytes_read == 0) {
                    read_eof_flag = true;
                } else if (stats.last_error_code.load() == 0) {
                    stats.last_error_code = errno;
                }
            }
            if (read_worker_needs_fill.load()) read_worker_needs_fill = false;
            read_cv_consumer.notify_all();
        }
    }
};
} // namespace libconveyor

conveyor_t* conveyor_create(const conveyor_config_t* cfg) {
    if (!cfg) { errno = EINVAL; return nullptr; }
    auto* impl = new libconveyor::ConveyorImpl(cfg->initial_write_size, cfg->initial_read_size);
    impl->handle = cfg->handle;
    impl->flags = cfg->flags;
    impl->ops = cfg->ops;
    impl->max_write_capacity = (cfg->max_write_size > 0) ? cfg->max_write_size : cfg->initial_write_size;
    impl->max_read_capacity  = (cfg->max_read_size > 0)  ? cfg->max_read_size  : cfg->initial_read_size;
    
    int mode = cfg->flags & O_ACCMODE;
    impl->read_buffer_enabled = (mode == O_RDONLY || mode == O_RDWR) && (cfg->initial_read_size > 0);
    impl->write_buffer_enabled = (mode == O_WRONLY || mode == O_RDWR) && (cfg->initial_write_size > 0);

    if (impl->read_buffer_enabled) {
        impl->read_worker_thread = std::thread(&libconveyor::ConveyorImpl::readWorker, impl);
        std::unique_lock<std::mutex> lock(impl->read_mutex);
        impl->read_worker_needs_fill = true;
        impl->read_cv_producer.notify_one();
    }
    if (impl->write_buffer_enabled) {
        if (impl->flags & O_APPEND) {
            off_t sz = impl->ops.lseek(impl->handle, 0, SEEK_END);
            if (sz == LIBCONVEYOR_ERROR) {
                conveyor_destroy(reinterpret_cast<conveyor_t*>(impl));
                return nullptr;
            }
            impl->logical_write_offset = sz;
            impl->current_file_offset = sz; 
        }
        impl->write_worker_thread = std::thread(&libconveyor::ConveyorImpl::writeWorker, impl);
    }
    return reinterpret_cast<conveyor_t*>(impl);
}

void conveyor_destroy(conveyor_t* conv) {
    if (!conv) return;
    auto* impl = reinterpret_cast<libconveyor::ConveyorImpl*>(conv);
    if (impl->write_buffer_enabled) conveyor_flush(conv);
    if (impl->read_buffer_enabled) {
        impl->read_worker_stop_flag = true;
        impl->read_cv_producer.notify_all();
        impl->read_cv_consumer.notify_all();
        if (impl->read_worker_thread.joinable()) impl->read_worker_thread.join();
    }
    if (impl->write_buffer_enabled) {
        impl->write_worker_stop_flag = true;
        impl->write_cv_producer.notify_all();
        impl->write_cv_consumer.notify_all();
        if (impl->write_worker_thread.joinable()) impl->write_worker_thread.join();
    }
    delete impl;
}

ssize_t conveyor_write(conveyor_t* conv, const void* buf, size_t count) {
    if (!conv) { errno = EBADF; return LIBCONVEYOR_ERROR; }
    auto* impl = reinterpret_cast<libconveyor::ConveyorImpl*>(conv);
    int mode = impl->flags & O_ACCMODE;
    if (mode != O_WRONLY && mode != O_RDWR) {
        errno = EBADF;
        return LIBCONVEYOR_ERROR;
    }
    
    if (count > impl->max_write_capacity) {
        errno = EMSGSIZE;
        return LIBCONVEYOR_ERROR;
    }

    if (!impl->write_buffer_enabled) { 
        return impl->ops.pwrite(impl->handle, buf, count, impl->current_file_offset.load());
    }

    if (impl->stats.last_error_code.load() != 0) { errno = impl->stats.last_error_code.load(); return LIBCONVEYOR_ERROR; }

    std::unique_lock<std::mutex> lock(impl->write_mutex);    
    
    // --- ADAPTIVE WRITE: Grow on Pressure ---
    if (impl->write_ring_buffer.available_space() < count) {
        if (impl->write_ring_buffer.capacity < impl->max_write_capacity) {
            // FLUSH LOGIC
            if (!impl->write_queue.empty()) {
                impl->write_buffer_needs_flush = true;
                impl->write_cv_consumer.notify_one();
                impl->write_cv_producer.wait(lock, [&] { return impl->write_queue.empty() || impl->write_worker_stop_flag; });
            }
            impl->write_buffer_needs_flush = false;

            size_t needed = impl->write_ring_buffer.size + count;
            size_t new_cap = impl->write_ring_buffer.capacity * 2;
            if (new_cap < needed) new_cap = needed;
            if (new_cap > impl->max_write_capacity) new_cap = impl->max_write_capacity;
            
            if (new_cap >= needed) impl->write_ring_buffer.resize(new_cap);
        }
    }
    // ----------------------------------------

    if (impl->write_ring_buffer.available_space() < count) impl->stats.write_buffer_full_events++;
    
    if(!impl->write_cv_producer.wait_for(lock, std::chrono::seconds(30), [&]{ 
        return (impl->write_ring_buffer.available_space() >= count) || impl->write_worker_stop_flag; 
    })) { errno = ETIMEDOUT; return LIBCONVEYOR_ERROR; }

    if (impl->write_worker_stop_flag) return LIBCONVEYOR_ERROR;

    size_t ring_pos_start = impl->write_ring_buffer.head;
    impl->write_ring_buffer.write(static_cast<const char*>(buf), count);
    
    libconveyor::WriteRequest req;
    req.file_offset = impl->current_file_offset.load();
    req.length = count;
    req.ring_buffer_pos = ring_pos_start;
    
    impl->write_queue.push_back(req);
    
    impl->current_file_offset += count;
    impl->stats.bytes_written += count;

    impl->write_cv_consumer.notify_one();
    return count;
}

ssize_t conveyor_read(conveyor_t* conv, void* buf, size_t count) {
    if (!conv) { errno = EBADF; return LIBCONVEYOR_ERROR; }
    auto* impl = reinterpret_cast<libconveyor::ConveyorImpl*>(conv);
    if (!impl->read_buffer_enabled) { errno = EBADF; return LIBCONVEYOR_ERROR; }
    if (impl->stats.last_error_code.load() != 0) { errno = impl->stats.last_error_code.load(); return LIBCONVEYOR_ERROR; }

    char* ptr = static_cast<char*>(buf);
    off_t start_offset = impl->current_file_offset.load();
    ssize_t total_read = 0;

    {
        std::unique_lock<std::mutex> read_lock(impl->read_mutex);
        
        // --- ADAPTIVE READ: Heuristic Logic ---
        bool grow = false;
        
        // Trigger 1: Request larger than buffer
        if (count > impl->read_buffer.capacity) grow = true;
        
        // Trigger 2: Sequential exhaustion (Buffer empty + Sequential Read)
        if (impl->read_buffer.empty() && start_offset == impl->last_read_end_offset) {
            impl->sequential_read_counter++;
            // If we hit empty buffer sequentially 3 times, assume bandwidth mismatch -> Grow
            if (impl->sequential_read_counter > 2) grow = true;
        } else {
            impl->sequential_read_counter = 0; // Reset on random seek
        }

        if (grow && impl->read_buffer.capacity < impl->max_read_capacity) {
            size_t new_cap = impl->read_buffer.capacity * 2;
            if (new_cap < count) new_cap = count; // Ensure we fit the specific request
            if (new_cap > impl->max_read_capacity) new_cap = impl->max_read_capacity;
            
            // Resize immediately. readWorker will see new capacity on next loop.
            impl->read_buffer.resize(new_cap);
        }
        impl->last_read_end_offset = start_offset + count;
        // ----------------------------------------

        off_t current_read_pos = start_offset;
        while (total_read < count && !impl->read_worker_stop_flag.load()) {
            if (impl->read_buffer.empty()) {
                if (impl->read_eof_flag.load() && (current_read_pos >= impl->read_head_in_storage.load())) break;
                impl->read_worker_needs_fill = true;
                impl->read_cv_producer.notify_one();
                impl->read_cv_consumer.wait(read_lock, [&]{ return impl->read_buffer.available_data() > 0 || impl->read_worker_stop_flag.load(); });
                if (impl->read_buffer.available_data() == 0) break;
            }
            size_t read_now = impl->read_buffer.read(ptr + total_read, count - total_read);
            total_read += read_now;
            current_read_pos += read_now;
            impl->read_cv_producer.notify_one();
        }
    } 

    if (impl->write_buffer_enabled) {
        std::unique_lock<std::mutex> write_lock(impl->write_mutex);
        off_t requested_read_end = start_offset + count;
        for (const auto& req : impl->write_queue) {
            off_t write_start = req.file_offset;
            off_t write_end = req.file_offset + req.length;
            off_t overlap_start = std::max(start_offset, write_start);
            off_t overlap_end = std::min(requested_read_end, write_end);
            if (overlap_start < overlap_end) {
                size_t len = static_cast<size_t>(overlap_end - overlap_start);
                size_t dest_idx = static_cast<size_t>(overlap_start - start_offset);
                size_t offset_in_req = static_cast<size_t>(overlap_start - write_start);
                size_t ring_abs_pos = req.ring_buffer_pos + offset_in_req;
                impl->write_ring_buffer.peek_at(ring_abs_pos, ptr + dest_idx, len);
                size_t bytes_covered = dest_idx + len;
                if (bytes_covered > total_read) total_read = bytes_covered;
            }
        }
    }

    impl->current_file_offset = start_offset + total_read;
    impl->stats.bytes_read += total_read;
    return total_read;
}

off_t conveyor_lseek(conveyor_t* conv, off_t offset, int whence) {
    if (!conv) { errno = EBADF; return LIBCONVEYOR_ERROR; }
    auto* impl = reinterpret_cast<libconveyor::ConveyorImpl*>(conv);

    if (impl->write_buffer_enabled) {
        int flush_result = conveyor_flush(conv);
        if (flush_result != 0) return LIBCONVEYOR_ERROR;
    }
    
    std::unique_lock<std::mutex> read_lock(impl->read_mutex, std::defer_lock);
    std::unique_lock<std::mutex> write_lock(impl->write_mutex, std::defer_lock);
    std::lock(read_lock, write_lock);

    off_t new_pos = impl->ops.lseek(impl->handle, offset, whence);

    if (new_pos != LIBCONVEYOR_ERROR) {
        if (impl->read_buffer_enabled) {
            impl->read_buffer.clear();
            impl->read_eof_flag = false;
            impl->read_buffer_generation++; 
            impl->read_cv_consumer.notify_all();
            impl->read_cv_producer.notify_all();
        }
        impl->current_file_offset = new_pos;
        impl->read_head_in_storage = new_pos;
        // Reset heuristics
        impl->sequential_read_counter = 0;
    }
    return new_pos;
}

int conveyor_flush(conveyor_t* conv) {
    if (!conv) { errno = EBADF; return LIBCONVEYOR_ERROR; }
    auto* impl = reinterpret_cast<libconveyor::ConveyorImpl*>(conv);
    if (!impl->write_buffer_enabled) return 0;
    if (impl->stats.last_error_code.load() != 0) { errno = impl->stats.last_error_code.load(); return LIBCONVEYOR_ERROR; }
    std::unique_lock<std::mutex> lock(impl->write_mutex);
    if (!impl->write_queue.empty()) {
        impl->write_buffer_needs_flush = true;
        impl->write_cv_consumer.notify_one();
        impl->write_cv_producer.wait(lock, [&] { return impl->write_queue.empty() || impl->write_worker_stop_flag; });
    }
    impl->write_buffer_needs_flush = false;
    if (impl->stats.last_error_code.load() != 0) { errno = impl->stats.last_error_code.load(); return LIBCONVEYOR_ERROR; }
    return 0;
}

int conveyor_get_stats(conveyor_t* conv, conveyor_stats_t* stats) {
    if (!conv || !stats) { errno = EBADF; return LIBCONVEYOR_ERROR; }
    auto* impl = reinterpret_cast<libconveyor::ConveyorImpl*>(conv);
    
    stats->bytes_written = impl->stats.bytes_written.exchange(0);
    stats->bytes_read = impl->stats.bytes_read.exchange(0);
    size_t w_latency = impl->stats.total_write_latency_ms.exchange(0);
    size_t w_ops = impl->stats.write_ops_count.exchange(0);
    size_t r_latency = impl->stats.total_read_latency_ms.exchange(0);
    size_t r_ops = impl->stats.read_ops_count.exchange(0);
    stats->write_buffer_full_events = impl->stats.write_buffer_full_events.exchange(0);
    stats->last_error_code = impl->stats.last_error_code.load();
    stats->avg_write_latency_ms = (w_ops > 0) ? (w_latency / w_ops) : 0;
    stats->avg_read_latency_ms = (r_ops > 0) ? (r_latency / r_ops) : 0;

    return 0;
}

void conveyor_stop(conveyor_t* conv) {
    if (!conv) return;
    auto* impl = reinterpret_cast<libconveyor::ConveyorImpl*>(conv);
    if (impl->read_buffer_enabled) {
        impl->read_worker_stop_flag = true;
        impl->read_cv_producer.notify_all();
        impl->read_cv_consumer.notify_all();
    }
    if (impl->write_buffer_enabled) {
        impl->write_worker_stop_flag = true;
        impl->write_cv_producer.notify_all();
        impl->write_cv_consumer.notify_all();
    }
}

int conveyor_clear_error(conveyor_t* conv) {
    if (!conv) {
        errno = EBADF;
        return LIBCONVEYOR_ERROR;
    }
    auto* impl = reinterpret_cast<libconveyor::ConveyorImpl*>(conv);
    impl->stats.last_error_code = 0;
    return 0;
}
