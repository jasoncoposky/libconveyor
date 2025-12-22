#include <gtest/gtest.h>
#include "libconveyor/conveyor.h"

#include <vector>
#include <cstring>
#include <mutex>
#include <thread>
#include <chrono>
#include <atomic>

// --- Mock Storage Backend ---
class MockStorage {
public:
    std::vector<char> data;
    std::mutex mx;

    MockStorage(size_t size) : data(size, 0) {}

    static ssize_t pwrite_callback(storage_handle_t h, const void* buf, size_t count, off_t offset) {
        auto* self = static_cast<MockStorage*>(h);
        std::lock_guard<std::mutex> lock(self->mx);
        if (offset + count > self->data.size()) {
            self->data.resize(offset + count);
        }
        std::memcpy(self->data.data() + offset, buf, count);
        return count;
    }

    static ssize_t pread_callback(storage_handle_t h, void* buf, size_t count, off_t offset) {
        auto* self = static_cast<MockStorage*>(h);
        std::lock_guard<std::mutex> lock(self->mx);
        if (offset >= (off_t)self->data.size()) return 0;
        size_t available = std::min((size_t)count, self->data.size() - (size_t)offset);
        std::memcpy(buf, self->data.data() + offset, available);
        return available;
    }

    static off_t lseek_callback(storage_handle_t h, off_t offset, int whence) {
        auto* self = static_cast<MockStorage*>(h);
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
class ConveyorConsistencyTest : public ::testing::Test {
protected:
    MockStorage* mock;
    conveyor_t* conv;
    
    void SetUp() override {
        mock = new MockStorage(1024);
        conv = nullptr;
    }

    void TearDown() override {
        if (conv) conveyor_destroy(conv);
        delete mock;
    }
};

// Verifies that a read after a flush gets the new data, not stale data from the read buffer.
TEST_F(ConveyorConsistencyTest, ReadFlushRead) {
    auto ops = mock->get_ops();
    conveyor_config_t cfg = {0};
    cfg.handle = mock;
    cfg.flags = O_RDWR;
    cfg.ops = ops;
    cfg.initial_write_size = 4096;
    cfg.initial_read_size = 4096;
    cfg.max_write_size = cfg.initial_write_size;
    cfg.max_read_size = cfg.initial_read_size;
    conv = conveyor_create(&cfg);
    ASSERT_NE(conv, nullptr);

    // 1. Write initial data to the mock storage.
    std::string old_data = "OLD_DATA";
    std::memcpy(mock->data.data(), old_data.c_str(), old_data.size());

    // 2. Read from the conveyor to fill the read buffer with "OLD_DATA".
    char read_buf[20] = {0};
    ssize_t res = conveyor_read(conv, read_buf, old_data.size());
    ASSERT_EQ(res, old_data.size());
    ASSERT_STREQ(read_buf, old_data.c_str());

    // 3. Write "NEW_DATA" to the same location in the conveyor.
    std::string new_data = "NEW_DATA";
    conveyor_lseek(conv, 0, SEEK_SET);
    res = conveyor_write(conv, new_data.c_str(), new_data.size());
    ASSERT_EQ(res, new_data.size());

    // 4. Flush the conveyor. This should write "NEW_DATA" to the mock storage.
    ASSERT_EQ(conveyor_flush(conv), 0);

    // 5. Read from the conveyor again. The read should return "NEW_DATA".
    std::memset(read_buf, 0, sizeof(read_buf));
    conveyor_lseek(conv, 0, SEEK_SET);
    res = conveyor_read(conv, read_buf, new_data.size());
    ASSERT_EQ(res, new_data.size());
    ASSERT_STREQ(read_buf, new_data.c_str());
}
