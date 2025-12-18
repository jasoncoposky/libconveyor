#include "libconveyor/conveyor.h"
#include "libconveyor/detail/ring_buffer.h"
#include <vector>
#include <mutex>
#include <condition_variable>
#include <thread>
#include <atomic>
#include <cerrno>
#include <cstring> // For memcpy
#include <algorithm>
#include <chrono>
#include <queue> 

#ifndef O_ACCMODE
#define O_ACCMODE (O_RDONLY | O_WRONLY | O_RDWR)
#endif

namespace libconveyor {



struct WriteRequest {
    std::vector<char> data;
    off_t offset;
};

struct ConveyorImpl {
    storage_handle_t handle;
    int flags;
    storage_operations_t ops;

    // Write Logic
    bool write_buffer_enabled = false;
    std::deque<WriteRequest> write_queue; // Changed to std::deque
    size_t write_buffer_capacity;
    std::atomic<size_t> write_queue_bytes{0};
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
    
    // --- FIX: GENERATION COUNTER ---
    // Prevents lseek/read race conditions
    std::atomic<uint64_t> read_buffer_generation{0}; 

    std::atomic<off_t> logical_write_offset{0};
    std::atomic<off_t> read_head_in_storage{0};
    std::atomic<off_t> current_file_offset{0};

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
        : write_buffer_capacity(w_cap), read_buffer(r_cap) {}

    void writeWorker() {
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

            WriteRequest req = write_queue.front();
            write_queue.pop_front();
            write_queue_bytes -= req.data.size();
            
            write_cv_producer.notify_all(); 

            lock.unlock(); 

            off_t write_pos;
            if (flags & O_APPEND) {
                write_pos = logical_write_offset.load(); 
            } else {
                write_pos = req.offset;
            }
            
            auto start = std::chrono::steady_clock::now();
            ssize_t written_bytes = ops.pwrite(handle, req.data.data(), req.data.size(), write_pos);
            auto end = std::chrono::steady_clock::now();
            
            lock.lock(); 
            stats.total_write_latency_ms += std::chrono::duration_cast<std::chrono::milliseconds>(end - start).count();
            stats.write_ops_count++;
            
            if (written_bytes < 0) {
                if (stats.last_error_code.load() == 0) stats.last_error_code = errno;
            } else if (written_bytes > 0) {
                if (flags & O_APPEND) { 
                    logical_write_offset += written_bytes;
                }
            }
        }
    }

    void readWorker() {
        std::vector<char> temp_buffer;
        temp_buffer.reserve(read_buffer.capacity);
        while (true) {
            std::unique_lock<std::mutex> lock(read_mutex);
            read_cv_producer.wait(lock, [&] { 
                return read_buffer.available_space() > 0 || read_worker_stop_flag.load() || read_worker_needs_fill.load(); 
            });
            if (read_worker_stop_flag.load()) break;

            if (read_buffer.available_space() > 0 && !read_worker_stop_flag.load()) {
                // Capture the generation BEFORE unlocking
                uint64_t my_gen = read_buffer_generation.load();
                
                off_t read_pos = current_file_offset + read_buffer.available_data();
                size_t n = read_buffer.available_space();
                temp_buffer.resize(n);
                
                lock.unlock(); // --- I/O STARTS ---

                auto start = std::chrono::steady_clock::now();
                ssize_t bytes_read = ops.pread(handle, temp_buffer.data(), n, read_pos);
                auto end = std::chrono::steady_clock::now();
                
                lock.lock(); // --- I/O ENDS ---

                // --- FIX: GENERATION CHECK ---
                if (my_gen != read_buffer_generation.load()) {
                    // World changed (lseek happened). Discard data.
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

conveyor_t* conveyor_create(storage_handle_t h, int f, const storage_operations_t* o, size_t w, size_t r) {
    auto* impl = new libconveyor::ConveyorImpl(w, r);
    impl->handle = h; impl->flags = f; impl->ops = *o;
    impl->write_buffer_capacity = w;
    int mode = f & O_ACCMODE;
    impl->read_buffer_enabled = (mode == O_RDONLY || mode == O_RDWR) && (r > 0);
    impl->write_buffer_enabled = (mode == O_WRONLY || mode == O_RDWR) && (w > 0);
    if (impl->read_buffer_enabled) {
        impl->read_worker_thread = std::thread(&libconveyor::ConveyorImpl::readWorker, impl);
        // Immediately signal readWorker to start filling the buffer
        std::unique_lock<std::mutex> lock(impl->read_mutex);
        impl->read_worker_needs_fill = true;
        impl->read_cv_producer.notify_one();
    }
    if (impl->write_buffer_enabled) {
        if (impl->flags & O_APPEND) {
            off_t initial_file_size = impl->ops.lseek(impl->handle, 0, SEEK_END);
            if (initial_file_size == LIBCONVEYOR_ERROR) {
                delete impl;
                // errno should be set by ops.lseek, propagate it
                return nullptr;
            }
            impl->logical_write_offset = initial_file_size;
            impl->current_file_offset = initial_file_size; 
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

    if (!impl->write_buffer_enabled) { // Unbuffered write path
        // Directly call the underlying pwrite operation
        ssize_t written_bytes = impl->ops.pwrite(impl->handle, buf, count, impl->current_file_offset.load());
        if (written_bytes > 0) {
            impl->current_file_offset += written_bytes;
            impl->stats.bytes_written += written_bytes;
        } else if (written_bytes == LIBCONVEYOR_ERROR) {
            // Propagate errno from the underlying pwrite call
            // No need to set errno here, it should already be set by mock_pwrite_fail_once
        }
        return written_bytes;
    }

    // Buffered write path (original logic)
    if (impl->stats.last_error_code.load() != 0) { errno = impl->stats.last_error_code.load(); return LIBCONVEYOR_ERROR; }

    if (count > impl->write_buffer_capacity) {
        errno = EMSGSIZE;
        return LIBCONVEYOR_ERROR;
    }
    
    std::unique_lock<std::mutex> lock(impl->write_mutex);    
    if (impl->write_queue_bytes.load() + count > impl->write_buffer_capacity) {
        impl->stats.write_buffer_full_events++;
    }
    
    if(!impl->write_cv_producer.wait_for(lock, std::chrono::seconds(30), [&]{ 
        return (impl->write_queue_bytes.load() + count <= impl->write_buffer_capacity) || impl->write_worker_stop_flag; 
    })) {
        errno = ETIMEDOUT;
        return LIBCONVEYOR_ERROR;
    }

    if (impl->write_worker_stop_flag) return LIBCONVEYOR_ERROR;

    libconveyor::WriteRequest req;
    req.data.assign(static_cast<const char*>(buf), static_cast<const char*>(buf) + count);
    req.offset = impl->current_file_offset.load();
    impl->write_queue.push_back(req);
    impl->write_queue_bytes += count;
    
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

    ssize_t total_read = 0;
    char* ptr = static_cast<char*>(buf);
    off_t current_read_offset_logic = impl->current_file_offset.load(); // Capture current logical offset

    // --- Phase 1: Try to satisfy read from unflushed writes (write_queue) ---
    if (impl->write_buffer_enabled) {
        std::unique_lock<std::mutex> write_lock(impl->write_mutex); // Lock for write_queue access
        for (const auto& req : impl->write_queue) {
            off_t write_start = req.offset;
            off_t write_end = req.offset + req.data.size();
            off_t read_start_current_iter = current_read_offset_logic + total_read;
            off_t read_end_current_iter = current_read_offset_logic + count;

            // Calculate overlap
            off_t overlap_start = std::max(read_start_current_iter, write_start);
            off_t overlap_end = std::min(read_end_current_iter, write_end);

            if (overlap_start < overlap_end) { // There is an overlap
                size_t bytes_from_write_queue = static_cast<size_t>(overlap_end - overlap_start);
                size_t offset_in_req_data = static_cast<size_t>(overlap_start - write_start);
                size_t offset_in_read_buf = static_cast<size_t>(overlap_start - current_read_offset_logic);
                
                std::memcpy(ptr + offset_in_read_buf, req.data.data() + offset_in_req_data, bytes_from_write_queue);
                total_read += bytes_from_write_queue;

                if (total_read == count) {
                    impl->current_file_offset = current_read_offset_logic + total_read;
                    impl->stats.bytes_read += total_read;
                    return total_read;
                }
            }
        }
    }

    // Release write_lock, no longer needed for reading
    // No, we already used a unique_lock, which will unlock when it goes out of scope.
    // So we need to create the read_lock before unlocking the write_lock.
    
    std::unique_lock<std::mutex> read_lock(impl->read_mutex); // Lock for read_buffer access
    // At this point, write_lock is released.

    current_read_offset_logic += total_read;
    size_t remaining_to_read = count - total_read;

    // --- Phase 2: Try to satisfy remaining read from read_buffer and underlying storage ---
    while (remaining_to_read > 0 && !impl->read_worker_stop_flag.load()) {
        if (impl->read_buffer.available_data() == 0) { // If read buffer is empty
            if (impl->read_eof_flag.load() && (current_read_offset_logic >= impl->read_head_in_storage.load())) {
                break; // Reached EOF and no more data in buffer or storage
            }

            impl->read_worker_needs_fill = true;
            impl->read_cv_producer.notify_one();

            // Wait for the read worker to fill the buffer
            // Use current_read_offset_logic to ensure read worker fills from correct position
            impl->read_cv_consumer.wait(read_lock, [&]{ 
                return (impl->read_buffer.available_data() > 0) || impl->read_worker_stop_flag.load();
            });

            if (impl->read_worker_stop_flag.load()) { // Check stop flag immediately
                break;
            }

            if (impl->read_buffer.available_data() == 0) { // Still no data after waiting
                if (impl->read_eof_flag.load() && (current_read_offset_logic >= impl->read_head_in_storage.load())) {
                    break; // Truly EOF
                } else if (impl->stats.last_error_code.load() != 0) {
                    errno = impl->stats.last_error_code.load();
                    return LIBCONVEYOR_ERROR; // Propagate error from worker
                }
                // If we reach here, it implies a logical issue or a timeout, return 0 for now.
                break;
            }
        }
        
        size_t bytes_from_read_buffer = impl->read_buffer.read(ptr + total_read, remaining_to_read);
        total_read += bytes_from_read_buffer;
        remaining_to_read -= bytes_from_read_buffer;
        current_read_offset_logic += bytes_from_read_buffer;
        impl->read_cv_producer.notify_one(); // Notify readWorker that space might be available in read_buffer
    }

    impl->current_file_offset = current_read_offset_logic; // Update global logical offset
    impl->stats.bytes_read += total_read;
    return total_read;
}

off_t conveyor_lseek(conveyor_t* conv, off_t offset, int whence) {
    if (!conv) { errno = EBADF; return LIBCONVEYOR_ERROR; }
    auto* impl = reinterpret_cast<libconveyor::ConveyorImpl*>(conv);

    if (impl->write_buffer_enabled) {
        int flush_result = conveyor_flush(conv);
        if (flush_result != 0) {
            // Error occurred during flush, propagate it.
            return LIBCONVEYOR_ERROR;
        }
    }
    
    // Now acquire locks for the seek operation itself
    std::unique_lock<std::mutex> read_lock(impl->read_mutex, std::defer_lock);
    std::unique_lock<std::mutex> write_lock(impl->write_mutex, std::defer_lock);
    std::lock(read_lock, write_lock);

    off_t new_pos = impl->ops.lseek(impl->handle, offset, whence);

    if (new_pos != LIBCONVEYOR_ERROR) {
        if (impl->read_buffer_enabled) {
            impl->read_buffer.clear();
            impl->read_eof_flag = false;
            
            // --- FIX: INVALIDATE WORKER GENERATION ---
            impl->read_buffer_generation++; 
            
            impl->read_cv_consumer.notify_all();
            impl->read_cv_producer.notify_all();
        }
        impl->current_file_offset = new_pos;
        impl->read_head_in_storage = new_pos;
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
    if (!conv || !stats) {
        errno = EBADF;
        return LIBCONVEYOR_ERROR;
    }
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