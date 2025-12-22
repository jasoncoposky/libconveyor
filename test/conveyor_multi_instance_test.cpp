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
class ConveyorMultiInstanceTest : public ::testing::Test {
protected:
    MockStorage* mock;
    conveyor_t* conv1;
    conveyor_t* conv2;
    
    void SetUp() override {
        mock = new MockStorage(1024);
        conv1 = nullptr;
        conv2 = nullptr;
    }

    void TearDown() override {
        if (conv1) conveyor_destroy(conv1);
        if (conv2) conveyor_destroy(conv2);
        delete mock;
    }
};

TEST_F(ConveyorMultiInstanceTest, OverlappingWrites) {
    auto ops = mock->get_ops();
    conveyor_config_t cfg1 = {0};
    cfg1.handle = mock;
    cfg1.flags = O_RDWR;
    cfg1.ops = ops;
    cfg1.initial_write_size = 1024;
    cfg1.initial_read_size = 0;
    cfg1.max_write_size = cfg1.initial_write_size;
    cfg1.max_read_size = cfg1.initial_read_size;
    conv1 = conveyor_create(&cfg1);

    conveyor_config_t cfg2 = {0};
    cfg2.handle = mock;
    cfg2.flags = O_RDWR;
    cfg2.ops = ops;
    cfg2.initial_write_size = 1024;
    cfg2.initial_read_size = 0;
    cfg2.max_write_size = cfg2.initial_write_size;
    cfg2.max_read_size = cfg2.initial_read_size;
    conv2 = conveyor_create(&cfg2);
    ASSERT_NE(conv1, nullptr);
    ASSERT_NE(conv2, nullptr);

    std::string data1(512, 'A');
    std::string data2(512, 'B');

    // Write data from both conveyors to the same offset
    conveyor_lseek(conv1, 0, SEEK_SET);
    conveyor_write(conv1, data1.c_str(), data1.size());

    conveyor_lseek(conv2, 0, SEEK_SET);
    conveyor_write(conv2, data2.c_str(), data2.size());

    // Flush both conveyors. The result is non-deterministic without O_APPEND,
    // but the final state should be consistent (either all 'A's or all 'B's).
    // The most likely outcome is that the second flush overwrites the first.
    conveyor_flush(conv1);
    conveyor_flush(conv2);

    std::string final_data(mock->data.data(), data1.size());
    // Check that the data is not interleaved
    bool all_A = true;
    bool all_B = true;
    for(char c : final_data) {
        if (c != 'A') all_A = false;
        if (c != 'B') all_B = false;
    }
    ASSERT_TRUE(all_A || all_B);
}
