#include "libconveyor/conveyor.h"
#include <vector>
#include <mutex>
#include <condition_variable>
#include <thread>
#include <atomic>
#include <cerrno>
#include <algorithm>

#ifndef O_ACCMODE
#define O_ACCMODE (O_RDONLY | O_WRONLY | O_RDWR)
#endif

// Placeholder for internal implementation details
namespace libconveyor {

// Ring buffer utility struct/class
struct RingBuffer {
    std::vector<char> buffer;
    size_t capacity = 0;
    size_t head = 0;
    size_t tail = 0;
    size_t size = 0;

    RingBuffer(size_t cap) : capacity(cap), buffer(cap) {}

    size_t write(const char* data, size_t len) {
        if (len == 0) return 0;
        size_t bytes_to_write = std::min(len, capacity - size);
        if (bytes_to_write == 0) return 0;

        size_t first_chunk_len = std::min(bytes_to_write, capacity - head);
        std::memcpy(buffer.data() + head, data, first_chunk_len);
        if (bytes_to_write > first_chunk_len) {
            std::memcpy(buffer.data(), data + first_chunk_len, bytes_to_write - first_chunk_len);
        }
        head = (head + bytes_to_write) % capacity;
        size += bytes_to_write;
        return bytes_to_write;
    }

    size_t read(char* data, size_t len) {
        if (len == 0) return 0;
        size_t bytes_to_read = std::min(len, size);
        if (bytes_to_read == 0) return 0;

        size_t first_chunk_len = std::min(bytes_to_read, capacity - tail);
        std::memcpy(data, buffer.data() + tail, first_chunk_len);
        if (bytes_to_read > first_chunk_len) {
            std::memcpy(data + first_chunk_len, buffer.data(), bytes_to_read - first_chunk_len);
        }
        tail = (tail + bytes_to_read) % capacity;
        size -= bytes_to_read;
        return bytes_to_read;
    }

    void clear() {
        head = 0;
        tail = 0;
        size = 0;
    }

    bool empty() const { return size == 0; }
    bool full() const { return size == capacity; }
    size_t available_space() const { return capacity - size; }
    size_t available_data() const { return size; }
};


// Internal structure to hold buffer data and state
struct ConveyorImpl {
    storage_handle_t handle;
    int flags;
    storage_operations_t ops;

    bool write_buffer_enabled = false;
    RingBuffer write_buffer;
    std::thread write_worker_thread;
    std::mutex write_mutex;
    std::condition_variable write_cv_producer;
    std::condition_variable write_cv_consumer;
    std::atomic<bool> write_worker_stop_flag{false};
    std::atomic<bool> write_buffer_needs_flush{false};
    std::atomic<off_t> current_pos_in_storage{0};

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

    off_t current_file_offset = 0;
    
    ConveyorImpl(size_t w_cap, size_t r_cap) 
        : write_buffer(w_cap), read_buffer(r_cap) {}

    void writeWorker() {
        std::vector<char> temp_flush_buffer;
        temp_flush_buffer.reserve(write_buffer.capacity);

        while (true) {
            std::unique_lock<std::mutex> lock(write_mutex);
            write_cv_consumer.wait(lock, [&] {
                return write_buffer.available_data() > 0 || write_buffer_needs_flush || write_worker_stop_flag;
            });

            if (write_worker_stop_flag && write_buffer.empty()) {
                break;
            }

            size_t bytes_to_flush = write_buffer.available_data();
            if (bytes_to_flush > 0) {
                temp_flush_buffer.resize(bytes_to_flush);
                write_buffer.read(temp_flush_buffer.data(), bytes_to_flush);
            }

            bool do_flush_now = write_buffer_needs_flush.load();
            if (do_flush_now) {
                write_buffer_needs_flush = false;
            }
            
            write_cv_producer.notify_all();

            if (bytes_to_flush > 0) {
                lock.unlock(); 
                
                if (flags & O_APPEND) {
                    ops.lseek(handle, 0, SEEK_END);
                }

                ssize_t written_to_storage = ops.write(handle, temp_flush_buffer.data(), bytes_to_flush);
                
                lock.lock(); 
                
                if (written_to_storage > 0) {
                    current_pos_in_storage += static_cast<off_t>(written_to_storage);
                }
            } else if (do_flush_now) {
                write_cv_producer.notify_all();
            }
        }
    }

    void readWorker() {
        std::vector<char> temp_read_buffer;
        temp_read_buffer.reserve(read_buffer.capacity);

        while (true) {
            std::unique_lock<std::mutex> lock(read_mutex);
            read_cv_producer.wait(lock, [&] {
                return read_buffer.available_space() > 0 || read_buffer_stale.load() || read_worker_stop_flag.load() || read_worker_needs_fill.load();
            });

            if (read_worker_stop_flag.load()) {
                break;
            }
            
            if (read_buffer_stale.load()) {
                read_buffer.clear();
                read_buffer_stale = false;
            }

            if (read_buffer.available_space() > 0 && !read_worker_stop_flag.load()) {
                if (current_pos_in_storage != (current_file_offset + read_buffer.available_data())) {
                    ops.lseek(handle, current_pos_in_storage, SEEK_SET);
                }

                size_t bytes_to_read_from_storage = read_buffer.available_space();
                temp_read_buffer.resize(bytes_to_read_from_storage);

                lock.unlock();
                ssize_t bytes_read_from_ops = ops.read(handle, temp_read_buffer.data(), bytes_to_read_from_storage);
                lock.lock(); 

                if (bytes_read_from_ops > 0) {
                    read_buffer.write(temp_read_buffer.data(), bytes_read_from_ops);
                    current_pos_in_storage += static_cast<off_t>(bytes_read_from_ops);
                } else if (bytes_read_from_ops == 0) {
                    read_eof_flag = true;
                }
            }
            
            if (read_worker_needs_fill.load()) {
                read_worker_needs_fill = false;
            }
            read_cv_consumer.notify_all();
        }
    }
};

} // namespace libconveyor

conveyor_t* conveyor_create(
    storage_handle_t handle,
    int flags,
    const storage_operations_t& ops,
    size_t write_buffer_size,
    size_t read_buffer_size)
{
    libconveyor::ConveyorImpl* impl = new libconveyor::ConveyorImpl(write_buffer_size, read_buffer_size);
    impl->handle = handle;
    impl->flags = flags;
    impl->ops = ops;

    int access_mode = flags & O_ACCMODE;

    if (access_mode == O_RDONLY || access_mode == O_RDWR) {
        impl->read_buffer_enabled = true;
    } else {
        impl->read_buffer_enabled = false;
    }

    if (access_mode == O_WRONLY || access_mode == O_RDWR) {
        impl->write_buffer_enabled = true;
    } else {
        impl->write_buffer_enabled = false;
    }

    if (impl->read_buffer_enabled) {
        impl->read_worker_thread = std::thread(&libconveyor::ConveyorImpl::readWorker, impl);
    }
    if (impl->write_buffer_enabled) {
        impl->write_worker_thread = std::thread(&libconveyor::ConveyorImpl::writeWorker, impl);
    }

    return reinterpret_cast<conveyor_t*>(impl);
}

void conveyor_destroy(conveyor_t* conv)
{
    if (!conv) return;

    libconveyor::ConveyorImpl* impl = reinterpret_cast<libconveyor::ConveyorImpl*>(conv);

    if (impl->write_buffer_enabled) {
        conveyor_flush(conv); 
    }

    if (impl->read_buffer_enabled) {
        impl->read_worker_stop_flag = true;
        impl->read_cv_producer.notify_all();
        impl->read_cv_consumer.notify_all();
        if (impl->read_worker_thread.joinable()) {
            impl->read_worker_thread.join();
        }
    }

    if (impl->write_buffer_enabled) {
        impl->write_worker_stop_flag = true;
        impl->write_cv_producer.notify_all();
        impl->write_cv_consumer.notify_all();
        if (impl->write_worker_thread.joinable()) {
            impl->write_worker_thread.join();
        }
    }
    
    delete impl;
}

ssize_t conveyor_write(conveyor_t* conv, const void* buf, size_t count)
{
    if (!conv) {
        errno = EBADF;
        return LIBCONVEYOR_ERROR;
    }

    libconveyor::ConveyorImpl* impl = reinterpret_cast<libconveyor::ConveyorImpl*>(conv);

    if (!impl->write_buffer_enabled) {
        errno = EBADF;
        return LIBCONVEYOR_ERROR;
    }

    ssize_t bytes_written_to_buffer = 0;
    const char* current_buf_ptr = static_cast<const char*>(buf);
    size_t remaining_count = count;

    {
        std::unique_lock<std::mutex> lock(impl->write_mutex);
        while (remaining_count > 0 && !impl->write_worker_stop_flag) {
            impl->write_cv_producer.wait(lock, [&] {
                return impl->write_buffer.available_space() > 0 || impl->write_worker_stop_flag;
            });

            if (impl->write_worker_stop_flag) {
                break;
            }

            size_t written_this_iter = impl->write_buffer.write(current_buf_ptr, remaining_count);
            current_buf_ptr += written_this_iter;
            remaining_count -= written_this_iter;
            bytes_written_to_buffer += written_this_iter;
            impl->current_file_offset += static_cast<off_t>(written_this_iter);

            impl->write_cv_consumer.notify_one();
        }
    }

    if (impl->read_buffer_enabled && (impl->flags & O_RDWR)) {
        std::lock_guard<std::mutex> lock(impl->read_mutex);
        impl->read_buffer.clear();
        impl->read_buffer_stale = true;
    }
    
    return bytes_written_to_buffer;
}

ssize_t conveyor_read(conveyor_t* conv, void* buf, size_t count)
{
    if (!conv) {
        errno = EBADF;
        return LIBCONVEYOR_ERROR;
    }

    libconveyor::ConveyorImpl* impl = reinterpret_cast<libconveyor::ConveyorImpl*>(conv);

    if (!impl->read_buffer_enabled) {
        errno = EBADF;
        return LIBCONVEYOR_ERROR;
    }

    std::unique_lock<std::mutex> lock(impl->read_mutex);

    ssize_t bytes_read_total = 0;
    char* current_buf_ptr = static_cast<char*>(buf);
    size_t remaining_count = count;

    while (remaining_count > 0 && !impl->read_worker_stop_flag.load()) {
        if (impl->read_buffer.empty()) {
            if (impl->read_eof_flag.load()) {
                break;
            }

            impl->read_worker_needs_fill = true;
            impl->read_cv_producer.notify_one();
            impl->read_cv_consumer.wait(lock, [&] {
                return impl->read_buffer.available_data() > 0 || impl->read_eof_flag.load() || impl->read_worker_stop_flag.load();
            });
            
            if (impl->read_buffer.available_data() == 0) {
                break;
            }
        }

        size_t bytes_to_read_this_iter = std::min(remaining_count, impl->read_buffer.available_data());
        size_t read_this_iter = impl->read_buffer.read(current_buf_ptr, bytes_to_read_this_iter);

        current_buf_ptr += read_this_iter;
        remaining_count -= read_this_iter;
        bytes_read_total += read_this_iter;
        impl->current_file_offset += static_cast<off_t>(read_this_iter);

        impl->read_cv_producer.notify_one();
    }
    
    return bytes_read_total;
}

off_t conveyor_lseek(conveyor_t* conv, off_t offset, int whence)
{
    if (!conv) {
        errno = EBADF;
        return LIBCONVEYOR_ERROR;
    }

    libconveyor::ConveyorImpl* impl = reinterpret_cast<libconveyor::ConveyorImpl*>(conv);

    std::lock(impl->read_mutex, impl->write_mutex);
    std::unique_lock<std::mutex> read_lock(impl->read_mutex, std::adopt_lock);
    std::unique_lock<std::mutex> write_lock(impl->write_mutex, std::adopt_lock);

    off_t new_pos = impl->ops.lseek(impl->handle, offset, whence);

    if (new_pos != LIBCONVEYOR_ERROR) {
        if (impl->read_buffer_enabled) {
            impl->read_buffer.clear();
            impl->read_buffer_stale = true;
            impl->read_eof_flag = false;
            impl->read_cv_consumer.notify_all();
            impl->read_cv_producer.notify_all();
        }
        if (impl->write_buffer_enabled) {
            impl->write_buffer.clear();
            impl->write_cv_consumer.notify_all();
        }
        
        impl->current_file_offset = new_pos;
        impl->current_pos_in_storage = new_pos;
    }

    return new_pos;
}

int conveyor_flush(conveyor_t* conv)
{
    if (!conv) {
        errno = EBADF;
        return LIBCONVEYOR_ERROR;
    }

    libconveyor::ConveyorImpl* impl = reinterpret_cast<libconveyor::ConveyorImpl*>(conv);

    if (!impl->write_buffer_enabled) {
        return 0;
    }

    std::unique_lock<std::mutex> lock(impl->write_mutex);

    if (impl->write_buffer.available_data() > 0) {
        impl->write_buffer_needs_flush = true;
        impl->write_cv_consumer.notify_one();

        impl->write_cv_producer.wait(lock, [&] {
            return impl->write_buffer.empty() || impl->write_worker_stop_flag;
        });
    }

    impl->write_buffer_needs_flush = false;

    return 0;
}
