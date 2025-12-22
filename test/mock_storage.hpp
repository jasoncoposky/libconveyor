#ifndef MOCK_STORAGE_HPP
#define MOCK_STORAGE_HPP

#include <vector>
#include <mutex>
#include <cstring>
#include <algorithm>
#include <thread>
#include <atomic>
#include "libconveyor/conveyor.h"

// Simulates a slow disk to force race conditions
struct MockStorage {
    std::vector<char> data;
    std::mutex mx;
    std::atomic<int> next_write_error{0};
    std::atomic<int> read_delay_ms{0};
    std::atomic<int> write_delay_ms{0};

    MockStorage(size_t size) : data(size, 0) {}

    static ssize_t pwrite_callback(storage_handle_t h, const void* buf, size_t count, off_t offset) {
        auto* self = reinterpret_cast<MockStorage*>(h);
        if (self->write_delay_ms > 0) std::this_thread::sleep_for(std::chrono::milliseconds(self->write_delay_ms));

        std::lock_guard<std::mutex> lock(self->mx);
        if (self->next_write_error != 0) {
            errno = self->next_write_error;
            self->next_write_error = 0;
            return -1;
        }
        if (offset + count > self->data.size()) self->data.resize(offset + count);
        std::memcpy(self->data.data() + offset, buf, count);
        return count;
    }

    static ssize_t pread_callback(storage_handle_t h, void* buf, size_t count, off_t offset) {
        auto* self = reinterpret_cast<MockStorage*>(h);
        if (self->read_delay_ms > 0) std::this_thread::sleep_for(std::chrono::milliseconds(self->read_delay_ms));

        std::lock_guard<std::mutex> lock(self->mx);
        if (offset >= (off_t)self->data.size()) return 0;
        size_t available = std::min(count, self->data.size() - offset);
        std::memcpy(buf, self->data.data() + offset, available);
        return available;
    }

    static off_t lseek_callback(storage_handle_t h, off_t offset, int whence) {
        auto* self = reinterpret_cast<MockStorage*>(h);
        std::lock_guard<std::mutex> lock(self->mx);
        if (whence == SEEK_SET) return offset;
        if (whence == SEEK_END) return self->data.size() + offset;
        return offset;
    }
    
    storage_operations_t get_ops() {
        return { pwrite_callback, pread_callback, lseek_callback };
    }
};

#endif
