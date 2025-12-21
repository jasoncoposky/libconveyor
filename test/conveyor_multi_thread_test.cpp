#include <gtest/gtest.h>
#include "libconveyor/conveyor.h"

#include <vector>
#include <cstring>
#include <mutex>
#include <thread>
#include <chrono>
#include <atomic>
#include <map>
#include <random> // For rand()

// --- Data Block for Stress Test ---
struct DataBlock {
    uint64_t sequence;
    uint32_t thread_id;
    uint32_t checksum;
    char data[4096 - 16]; // 4KB block size
};

// Simple checksum calculation
uint32_t calculate_checksum(const DataBlock& block) {
    uint32_t sum = 0;
    sum += static_cast<uint32_t>(block.sequence);
    sum += block.thread_id;
    for (size_t i = 0; i < sizeof(block.data); ++i) {
        sum += block.data[i];
    }
    return sum;
}

// --- Mock Storage Backend ---
// Simulates a slow disk to force race conditions
class MockStorage {
public:
    std::vector<char> data;
    std::mutex mx;
    std::atomic<int> next_write_error{0};
    std::atomic<int> read_delay_ms{0};
    std::atomic<int> write_delay_ms{0};

    MockStorage(size_t size) : data(size, 0) {}

    static ssize_t pwrite_callback(storage_handle_t h, const void* buf, size_t count, off_t offset) {
        auto* self = static_cast<MockStorage*>(h);
        
        if (self->write_delay_ms > 0) {
            std::this_thread::sleep_for(std::chrono::milliseconds(self->write_delay_ms));
        }

        std::lock_guard<std::mutex> lock(self->mx);

        if (self->next_write_error != 0) {
            errno = self->next_write_error;
            self->next_write_error = 0; // Clear after triggering
            return -1;
        }

        if (offset + count > self->data.size()) {
            self->data.resize(offset + count);
        }
        std::memcpy(self->data.data() + offset, buf, count);
        return count;
    }

    static ssize_t pread_callback(storage_handle_t h, void* buf, size_t count, off_t offset) {
        auto* self = static_cast<MockStorage*>(h);
        
        // Critical for testing the "Lseek Race": allow reading to be slow
        if (self->read_delay_ms > 0) {
            std::this_thread::sleep_for(std::chrono::milliseconds(self->read_delay_ms));
        }

        std::lock_guard<std::mutex> lock(self->mx);
        
        if (offset >= (off_t)self->data.size()) return 0;
        
        size_t available = std::min((size_t)count, self->data.size() - (size_t)offset);
        std::memcpy(buf, self->data.data() + offset, available);
        return available;
    }

    static off_t lseek_callback(storage_handle_t h, off_t offset, int whence) {
        auto* self = static_cast<MockStorage*>(h);
        std::lock_guard<std::mutex> lock(self->mx);
        // Simplified lseek implementation
        if (whence == SEEK_SET) return offset;
        if (whence == SEEK_END) return static_cast<off_t>(self->data.size()) + offset;
        return offset; // SEEK_CUR not supported in this mock for simplicity
    }
    
    storage_operations_t get_ops() {
        return { pwrite_callback, pread_callback, lseek_callback };
    }
};

// --- Test Fixture ---
class ConveyorMultiThreadTest : public ::testing::Test {
protected:
    MockStorage* mock;
    conveyor_t* conv;
    
    void SetUp() override {
        mock = new MockStorage(1024 * 1024); // 1MB Fake Disk
        conv = nullptr; // Initialize to nullptr
    }

    void TearDown() override {
        if (conv) conveyor_destroy(conv);
        delete mock;
    }
};


// --- Multi-threaded Application Stress Test ---
// Verifies that the public API is thread-safe when called from multiple app threads.
TEST_F(ConveyorMultiThreadTest, ConcurrentReadWrite) {
    auto ops = mock->get_ops();
    conv = conveyor_create(mock, O_RDWR, &ops, 1024 * 1024, 1024 * 1024); // 1MB buffers
    ASSERT_NE(conv, nullptr);

    std::atomic<bool> stop_flag = false;
    std::atomic<bool> test_failed = false;
    std::atomic<uint64_t> total_bytes_written = 0;

    const int num_writer_threads = 2;
    const int num_reader_threads = 2;
    const int test_duration_secs = 5;

    std::vector<std::thread> threads;

    // --- Create Writer Threads ---
    for (int i = 0; i < num_writer_threads; ++i) {
        threads.emplace_back([this, &stop_flag, &total_bytes_written, i]() {
            uint64_t seq = 0;
            while (!stop_flag) {
                DataBlock block;
                block.sequence = seq++;
                block.thread_id = i;
                std::memset(block.data, 'A' + (i % 26), sizeof(block.data));
                block.checksum = calculate_checksum(block);

                ssize_t res = conveyor_write(conv, &block, sizeof(block));
                if (res > 0) {
                    total_bytes_written += res;
                }
            }
        });
    }

    // --- Create Reader Threads ---
    for (int i = 0; i < num_reader_threads; ++i) {
        threads.emplace_back([this, &stop_flag, &test_failed, &total_bytes_written]() {
            DataBlock block;
            while (!stop_flag) {
                // Seek to a random valid position to read from
                uint64_t current_total_written = total_bytes_written.load();
                if (current_total_written < sizeof(DataBlock)) {
                    std::this_thread::yield();
                    continue;
                }
                
                // Ensure read_pos is within a valid block boundary
                off_t max_read_offset = current_total_written - sizeof(DataBlock);
                if (max_read_offset < 0) { // Should not happen if current_total_written >= sizeof(DataBlock)
                    std::this_thread::yield();
                    continue;
                }
                
                off_t read_pos = (rand() % (max_read_offset / sizeof(DataBlock) + 1)) * sizeof(DataBlock);
                conveyor_lseek(conv, read_pos, SEEK_SET);

                ssize_t res = conveyor_read(conv, &block, sizeof(block));
                if (res == sizeof(block)) {
                    uint32_t expected_checksum = calculate_checksum(block);
                    if (block.checksum != expected_checksum) {
                        test_failed = true;
                        break; 
                    }
                } else if (res == -1) {
                    // Handle read errors (e.g., if conveyor_read fails)
                    // For now, simply skip to next iteration
                }
            }
        });
    }

    // Let the threads run for a while
    std::this_thread::sleep_for(std::chrono::seconds(test_duration_secs));
    stop_flag = true;

    for (auto& t : threads) {
        t.join();
    }

    ASSERT_FALSE(test_failed) << "Data corruption detected by a reader thread.";
}
