#include "libconveyor/conveyor.h"
#include <iostream>
#include <vector>
#include <mutex>
#include <condition_variable>
#include <thread>
#include <atomic>
#include <cerrno>
#include <algorithm>
#include <chrono>
#include <queue> // For std::queue
#include "libconveyor/detail/ring_buffer.h"

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

    bool write_buffer_enabled = false;
    std::deque<WriteRequest> write_queue; // Changed from std::queue to std::deque
    size_t write_buffer_capacity; // To enforce limits on write_queue size
    std::atomic<size_t> write_queue_bytes{0}; // Track total bytes in queue
    std::thread write_worker_thread;
    std::mutex write_mutex;
    std::condition_variable write_cv_producer;
    std::condition_variable write_cv_consumer;
    std::atomic<bool> write_worker_stop_flag{false};
    std::atomic<bool> write_buffer_needs_flush{false};
    
    bool read_buffer_enabled = false;
    RingBuffer read_buffer;
    std::thread read_worker_thread;
    std::mutex read_mutex;
    std::condition_variable read_cv_producer;
    std::condition_variable read_cv_consumer;
    std::atomic<bool> read_worker_stop_flag{false};
    std::atomic<bool> read_buffer_stale{false};
    std::atomic<bool> read_worker_needs_fill{false};
    std::atomic<bool> read_eof_flag{false};

    std::atomic<off_t> logical_write_offset{0}; // Tracks the logical end of file after writes
    std::atomic<off_t> read_head_in_storage{0};  // Tracks the physical read position for readWorker
    std::atomic<off_t> current_file_offset{0}; // Tracks the logical current file offset for all operations

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
    
    // Constructor
    ConveyorImpl(size_t w_cap, size_t r_cap) 
        : write_buffer_capacity(w_cap), 
          read_buffer(r_cap) {} // Initialize read_buffer here

    // Member function declarations for worker threads
    void writeWorker();
    void readWorker();
}; // End of ConveyorImpl struct definition

// Move implementations outside the struct
void ConveyorImpl::writeWorker() {
    while (true) {
        std::unique_lock<std::mutex> lock(write_mutex);
        write_cv_consumer.wait(lock, [&] { 
            return !write_queue.empty() || write_buffer_needs_flush || write_worker_stop_flag; 
        });
        if (write_worker_stop_flag && write_queue.empty()) break;

                    // Process all items in the queue
                    while (!write_queue.empty()) {
                        WriteRequest req = write_queue.front();
                        write_queue.pop_front();
                        // write_queue_bytes -= req.data.size(); // OLD POSITION - MOVED BELOW
                        
                        lock.unlock(); // Unlock before performing I/O
        
                        // Handle O_APPEND semantics here, before pwrite
                        off_t write_pos = req.offset;
                        if (flags & O_APPEND) {
                            // For append mode, seek to end *just before* writing to ensure data is appended
                            // It's important to use the underlying lseek directly here.
                            write_pos = ops.lseek(handle, 0, SEEK_END); 
                            if (write_pos == LIBCONVEYOR_ERROR) {
                                lock.lock(); // Re-lock to update stats.last_error_code
                                if (stats.last_error_code.load() == 0) stats.last_error_code = errno;
                                lock.unlock();
                                // If lseek fails, we cannot proceed with this write.
                                // We must re-lock to update state if necessary and then continue;
                                // For now, continue to next request.
                                continue; 
                            }
                        }
                        
                        auto start = std::chrono::steady_clock::now();
                        ssize_t written_bytes = ops.pwrite(handle, req.data.data(), req.data.size(), write_pos);
                        auto end = std::chrono::steady_clock::now();
                        
                        // Update atomics outside the lock
                        stats.total_write_latency_ms += std::chrono::duration_cast<std::chrono::milliseconds>(end - start).count();
                        stats.write_ops_count++;                            
                        lock.lock(); // Re-lock only for updating error code and logical_write_offset based on success/failure
                        if (written_bytes < 0) {
                            if (stats.last_error_code.load() == 0) stats.last_error_code = errno;
                        } else if (written_bytes > 0) {
                            logical_write_offset = write_pos + static_cast<off_t>(written_bytes);
                        }
                        write_queue_bytes -= req.data.size(); // NEW POSITION - Decrement bytes count AFTER pwrite
                        write_cv_producer.notify_all(); // Notify producers that space is available
                    }        
        // After processing all items, if a flush was requested and queue is empty, notify.
        if (write_buffer_needs_flush && write_queue.empty()) {
            write_buffer_needs_flush = false;
            write_cv_producer.notify_all(); // Unblock anyone waiting for empty queue (e.g., conveyor_lseek, conveyor_flush)
        }        
    }
}

void ConveyorImpl::readWorker() {
    std::vector<char> temp_buffer;
    temp_buffer.reserve(read_buffer.capacity); // Reserve once

    while (true) {
        std::unique_lock<std::mutex> lock(read_mutex);
        // Wait for signal: buffer stale, stop, or read_worker_needs_fill
        read_cv_producer.wait(lock, [&] { 
            return read_buffer_stale.load() || read_worker_stop_flag.load() || read_worker_needs_fill.load(); 
        });

        if (read_worker_stop_flag.load()) break;

        if (read_buffer_stale.load()) {
            read_buffer.clear();
            read_buffer_stale = false;
            // After clearing, reset read_head_in_storage to current_file_offset if we intend to fill from there
            // This will be handled by conveyor_lseek setting read_head_in_storage, so no action here.
            // Just clear the buffer and continue waiting for an explicit fill request.
        }
        
        // Only attempt to fill the buffer if requested and there is space
        if (read_worker_needs_fill.load() && read_buffer.available_space() > 0) {
            // read_worker_needs_fill = false; // Reset the flag early to avoid re-triggering -- REMOVED
            off_t read_pos = read_head_in_storage.load();
            size_t n = read_buffer.available_space();
            temp_buffer.resize(n); // Resize temp_buffer to match what we intend to read
            
            lock.unlock(); // Unlock before performing I/O

            auto start = std::chrono::steady_clock::now();
            ssize_t bytes_read = ops.pread(handle, temp_buffer.data(), n, read_pos);
            auto end = std::chrono::steady_clock::now();
            
            lock.lock(); // Re-lock to update shared state
            stats.total_read_latency_ms += std::chrono::duration_cast<std::chrono::milliseconds>(end - start).count();
            stats.read_ops_count++;

            if (bytes_read > 0) {
                read_buffer.write(temp_buffer.data(), bytes_read);
                read_head_in_storage += static_cast<off_t>(bytes_read);
            } else if (bytes_read == 0) {
                read_eof_flag = true;
            } else if (stats.last_error_code.load() == 0) {
                stats.last_error_code = errno;
            }
            read_worker_needs_fill = false; // Reset the flag after processing
        }
        read_cv_consumer.notify_all(); // Notify consumers that buffer state might have changed
    }
}
} // End namespace libconveyor

conveyor_t* conveyor_create(storage_handle_t h, int f, const storage_operations_t* ops, size_t w, size_t r) {
    auto* impl = new libconveyor::ConveyorImpl(w, r);
    impl->handle = h; impl->flags = f; impl->ops = *ops;
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
    if (impl->write_buffer_enabled) impl->write_worker_thread = std::thread(&libconveyor::ConveyorImpl::writeWorker, impl);
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
    
    // Acquire both locks to ensure consistency
    std::unique_lock<std::mutex> read_lock(impl->read_mutex, std::defer_lock);
    std::unique_lock<std::mutex> write_lock(impl->write_mutex, std::defer_lock);
    std::lock(read_lock, write_lock);

    ssize_t total_read = 0;
    char* ptr = static_cast<char*>(buf);
    off_t current_read_offset = impl->current_file_offset.load();

    // --- Phase 1: Try to satisfy read from unflushed writes (write_queue) ---
    if (impl->write_buffer_enabled) {
        for (const auto& req : impl->write_queue) {
            off_t write_start = req.offset;
            off_t write_end = req.offset + req.data.size();
            off_t read_start_current_iter = current_read_offset + total_read;
            off_t read_end_current_iter = current_read_offset + count;

            // Calculate overlap
            off_t overlap_start = std::max(read_start_current_iter, write_start);
            off_t overlap_end = std::min(read_end_current_iter, write_end);

            if (overlap_start < overlap_end) { // There is an overlap
                size_t bytes_from_write_queue = static_cast<size_t>(overlap_end - overlap_start);
                size_t offset_in_req_data = static_cast<size_t>(overlap_start - write_start);
                
                std::memcpy(ptr + total_read, req.data.data() + offset_in_req_data, bytes_from_write_queue);
                total_read += bytes_from_write_queue;

                if (total_read == count) {
                    break; // All requested bytes satisfied from write_queue
                }
            }
        }
    }

    // Update current_read_offset based on what was read from write_queue
    current_read_offset += total_read;
    size_t remaining_count = count - total_read;

    // --- Phase 2: Try to satisfy remaining read from read_buffer and underlying storage ---
    while (remaining_count > 0 && !impl->read_worker_stop_flag.load()) {
        if (impl->read_buffer.available_data() == 0) { // If read buffer is empty
            if (impl->read_eof_flag.load() && (current_read_offset >= impl->read_head_in_storage.load())) {
                break; // Reached EOF and no more data in buffer or storage
            }

            impl->read_worker_needs_fill = true;
            impl->read_cv_producer.notify_one();

            // Wait for the read worker to fill the buffer
            // Use current_read_offset to ensure read worker fills from correct position
            impl->read_cv_consumer.wait(read_lock, [&]{ 
                return (impl->read_buffer.available_data() > 0) || impl->read_worker_stop_flag.load();
            });

            if (impl->read_worker_stop_flag.load()) { // Check stop flag immediately
                break;
            }

            if (impl->read_buffer.available_data() == 0) { // Still no data after waiting
                if (impl->read_eof_flag.load() && (current_read_offset >= impl->read_head_in_storage.load())) {
                    break; // Truly EOF
                } else if (impl->stats.last_error_code.load() != 0) {
                    errno = impl->stats.last_error_code.load();
                    return LIBCONVEYOR_ERROR; // Propagate error from worker
                }
                // If we reach here, it implies a logical issue or a timeout, return 0 for now.
                // This could also be a spurious wakeup where buffer is still empty and not EOF.
                // In a robust system, we might loop back or indicate a transient error.
                // For now, break and return what we have (0 in this case).
                break;
            }
        }
        
        size_t bytes_from_read_buffer = impl->read_buffer.read(ptr + total_read, remaining_count);
        total_read += bytes_from_read_buffer;
        remaining_count -= bytes_from_read_buffer;
        current_read_offset += bytes_from_read_buffer;
        impl->read_cv_producer.notify_one(); // Notify readWorker that space might be available in read_buffer
    }

    impl->current_file_offset = current_read_offset; // Update global logical offset
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
            impl->read_buffer_stale = true;
            impl->read_eof_flag = false;
            impl->read_cv_consumer.notify_all();
            impl->read_cv_producer.notify_all();
            impl->read_head_in_storage = new_pos;
        }
        impl->current_file_offset = new_pos;
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