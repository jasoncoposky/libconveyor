#ifndef LIBCONVEYOR_DETAIL_RING_BUFFER_H
#define LIBCONVEYOR_DETAIL_RING_BUFFER_H

#include <vector>
#include <cstddef> // For size_t
#include <algorithm> // For std::min
#include <cstring> // For std::memcpy

namespace libconveyor {

struct RingBuffer { // Still needed for read buffer
    std::vector<char> buffer;
    size_t capacity = 0;
    size_t head = 0;
    size_t tail = 0;
    size_t size = 0;

    RingBuffer(size_t cap) : capacity(cap), buffer(cap) {}

    // --- NEW: Resize Method ---
    // Unrolls the circular buffer into a new larger linear buffer.
    // Thread-Safety: Must be called under lock.
    void resize(size_t new_capacity) {
        if (new_capacity <= capacity) return; // Only support growing

        std::vector<char> new_buffer(new_capacity);

        if (size > 0) {
            if (head > tail) {
                // Data is in a single contiguous block
                std::memcpy(new_buffer.data(), buffer.data() + tail, size);
            } else {
                // Data is in two parts (wrapped)
                size_t first_chunk = capacity - tail;
                std::memcpy(new_buffer.data(), buffer.data() + tail, first_chunk);
                std::memcpy(new_buffer.data() + first_chunk, buffer.data(), head);
            }
        }

        buffer = std::move(new_buffer);
        capacity = new_capacity;
        head = size;
        tail = 0;
    }

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
        
        if (data != nullptr) {
            size_t first_chunk_len = std::min(bytes_to_read, capacity - tail);
            std::memcpy(data, buffer.data() + tail, first_chunk_len);
            if (bytes_to_read > first_chunk_len) {
                std::memcpy(data + first_chunk_len, buffer.data(), bytes_to_read - first_chunk_len);
            }
        }
        
        tail = (tail + bytes_to_read) % capacity;
        size -= bytes_to_read;
        return bytes_to_read;
    }

    // Helper to peek at data without advancing 'tail' (for snooping)
    // Returns bytes copied.
    size_t peek_at(size_t absolute_ring_pos, char* dest, size_t len) const {
        size_t offset = absolute_ring_pos % capacity;
        size_t first_chunk = std::min(len, capacity - offset);
        std::memcpy(dest, buffer.data() + offset, first_chunk);
        if (len > first_chunk) {
            std::memcpy(dest + first_chunk, buffer.data(), len - first_chunk);
        }
        return len;
    }

    void clear() { size = 0; head = 0; tail = 0; }
    bool empty() const { return size == 0; }
    bool full() const { return size == capacity; }
    size_t available_space() const { return capacity - size; }
    size_t available_data() const { return size; }
};

} // namespace libconveyor

#endif // LIBCONVEYOR_DETAIL_RING_BUFFER_H