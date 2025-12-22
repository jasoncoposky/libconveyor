#include <gtest/gtest.h>
#include "libconveyor/conveyor.h"

#include <vector>
#include <cstring>
#include <mutex>
#include <thread>
#include <chrono>
#include <atomic>

// --- Mock Storage Backend for Partial I/O ---
class MockPartialIOStorage {
public:
    std::vector<char> data;
    std::mutex mx;
    std::atomic<size_t> pwrite_partial_bytes{0}; // Bytes to write on first call
    std::atomic<size_t> pread_partial_bytes{0};  // Bytes to read on first call
    std::atomic<int> pwrite_call_count{0};
    std::atomic<int> pread_call_count{0};

    MockPartialIOStorage(size_t size) : data(size, 0) {}

    static ssize_t pwrite_callback(storage_handle_t h, const void* buf, size_t count, off_t offset) {
        auto* self = static_cast<MockPartialIOStorage*>(h);
        std::lock_guard<std::mutex> lock(self->mx);

        if (offset + count > self->data.size()) {
            self->data.resize(offset + count);
        }

        size_t bytes_to_write = count;
        if (self->pwrite_call_count.fetch_add(1) == 0 && self->pwrite_partial_bytes > 0) {
            bytes_to_write = self->pwrite_partial_bytes;
        }

        std::memcpy(self->data.data() + offset, buf, bytes_to_write);
        return bytes_to_write;
    }

    static ssize_t pread_callback(storage_handle_t h, void* buf, size_t count, off_t offset) {
        auto* self = static_cast<MockPartialIOStorage*>(h);
        std::lock_guard<std::mutex> lock(self->mx);

        if (offset >= (off_t)self->data.size()) return 0;

        size_t available = std::min((size_t)count, self->data.size() - (size_t)offset);
        
        size_t bytes_to_read = available;
        if (self->pread_call_count.fetch_add(1) == 0 && self->pread_partial_bytes > 0) {
            bytes_to_read = std::min(available, self->pread_partial_bytes.load());
        }

        std::memcpy(buf, self->data.data() + offset, bytes_to_read);
        return bytes_to_read;
    }

    static off_t lseek_callback(storage_handle_t h, off_t offset, int whence) {
        auto* self = static_cast<MockPartialIOStorage*>(h);
        std::lock_guard<std::mutex> lock(self->mx);
        if (whence == SEEK_SET) return offset;
        if (whence == SEEK_END) return static_cast<off_t>(self->data.size()) + offset;
        return offset;
    }
    
    storage_operations_t get_ops() {
        return { pwrite_callback, pread_callback, lseek_callback };
    }
};

// --- Test Fixture ---
class ConveyorPartialIOTest : public ::testing::Test {
protected:
    MockPartialIOStorage* mock;
    conveyor_t* conv;
    
    void SetUp() override {
        mock = new MockPartialIOStorage(1024);
        conv = nullptr;
    }

    void TearDown() override {
        if (conv) conveyor_destroy(conv);
        delete mock;
    }
};

TEST_F(ConveyorPartialIOTest, PartialWrite) {
    auto ops = mock->get_ops();
    conveyor_config_t cfg = {0};
    cfg.handle = mock;
    cfg.flags = O_WRONLY;
    cfg.ops = ops;
    cfg.initial_write_size = 4096;
    cfg.initial_read_size = 0;
    cfg.max_write_size = cfg.initial_write_size;
    cfg.max_read_size = cfg.initial_read_size;
    conv = conveyor_create(&cfg);
    ASSERT_NE(conv, nullptr);

    std::string data = "ThisIsAUnitTestForPartialWrites";
    
    // Configure mock to only write 10 bytes on the first call
    mock->pwrite_partial_bytes = 10;
    
    ssize_t res = conveyor_write(conv, data.c_str(), data.size());
    ASSERT_EQ(res, data.size());

    ASSERT_EQ(conveyor_flush(conv), 0);

    // Check if the full data was eventually written.
    ASSERT_EQ(std::string(mock->data.data(), data.size()), data);
}

TEST_F(ConveyorPartialIOTest, PartialRead) {
    auto ops = mock->get_ops();
    conveyor_config_t cfg = {0};
    cfg.handle = mock;
    cfg.flags = O_RDONLY;
    cfg.ops = ops;
    cfg.initial_write_size = 0;
    cfg.initial_read_size = 4096;
    cfg.max_write_size = cfg.initial_write_size;
    cfg.max_read_size = cfg.initial_read_size;
    conv = conveyor_create(&cfg);
    ASSERT_NE(conv, nullptr);

    std::string data = "ThisIsAUnitTestForPartialReads";
    std::memcpy(mock->data.data(), data.c_str(), data.size());

    // Configure mock to only read 10 bytes on the first call
    mock->pread_partial_bytes = 10;
    
    char read_buf[100] = {0};
    ssize_t res = conveyor_read(conv, read_buf, data.size());

    // The current `readWorker` and `conveyor_read` should loop to get all data.
    ASSERT_EQ(res, data.size());
    ASSERT_STREQ(read_buf, data.c_str());
}
