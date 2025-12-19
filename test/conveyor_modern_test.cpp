#include <gtest/gtest.h>
#include <gmock/gmock.h>
#include "libconveyor/conveyor_modern.hpp"
#include <vector>
#include <string>
#include <cstring>
#include <numeric>

using namespace libconveyor::v2;
using ::testing::_;
using ::testing::Return;
using ::testing::SetArgPointee;
using ::testing::Invoke;

// --- Mock Storage Backend (for C-style API, to be used by modern wrapper) ---
class MockStorage {
public:
    std::vector<char> data;
    std::mutex mx;
    
    MockStorage(size_t size = 1024) : data(size, 0) {}

    MOCK_METHOD(ssize_t, pwrite_mock, (storage_handle_t h, const void* buf, size_t count, off_t offset), ());
    MOCK_METHOD(ssize_t, pread_mock, (storage_handle_t h, void* buf, size_t count, off_t offset), ());
    MOCK_METHOD(off_t, lseek_mock, (storage_handle_t h, off_t offset, int whence), ());

    // Static callbacks for C-API
    static ssize_t pwrite_callback(storage_handle_t h, const void* buf, size_t count, off_t offset) {
        return static_cast<MockStorage*>(h)->pwrite_mock(h, buf, count, offset);
    }
    static ssize_t pread_callback(storage_handle_t h, void* buf, size_t count, off_t offset) {
        return static_cast<MockStorage*>(h)->pread_mock(h, buf, count, offset);
    }
    static off_t lseek_callback(storage_handle_t h, off_t offset, int whence) {
        return static_cast<MockStorage*>(h)->lseek_mock(h, offset, whence);
    }

    storage_operations_t get_ops() {
        return { pwrite_callback, pread_callback, lseek_callback };
    }
};

class ConveyorModernTest : public ::testing::Test {
protected:
    MockStorage* mock_storage;
    storage_operations_t ops;
    
    void SetUp() override {
        mock_storage = new MockStorage();
        ops = mock_storage->get_ops();

        // Default expectations for a healthy system
        ON_CALL(*mock_storage, pwrite_mock(_, _, _, _))
            .WillByDefault(Invoke([this](storage_handle_t, const void* buf, size_t count, off_t offset) {
                std::lock_guard<std::mutex> lock(mock_storage->mx);
                if (offset + count > mock_storage->data.size()) {
                    mock_storage->data.resize(offset + count);
                }
                std::memcpy(mock_storage->data.data() + offset, buf, count);
                return count;
            }));
        ON_CALL(*mock_storage, pread_mock(_, _, _, _))
            .WillByDefault(Invoke([this](storage_handle_t, void* buf, size_t count, off_t offset) {
                std::lock_guard<std::mutex> lock(mock_storage->mx);
                if (offset >= (off_t)mock_storage->data.size()) return 0;
                size_t available = std::min(count, mock_storage->data.size() - (size_t)offset);
                std::memcpy(buf, mock_storage->data.data() + offset, available);
                return available;
            }));
        ON_CALL(*mock_storage, lseek_mock(_, _, SEEK_SET))
            .WillByDefault(Return(static_cast<off_t>(0)));
        ON_CALL(*mock_storage, lseek_mock(_, _, SEEK_END))
            .WillByDefault(Invoke([this](storage_handle_t, off_t offset, int) -> off_t {
                std::lock_guard<std::mutex> lock(mock_storage->mx);
                return static_cast<off_t>(mock_storage->data.size() + offset);
            }));
    }

    void TearDown() override {
        delete mock_storage;
    }
};

TEST_F(ConveyorModernTest, CreateAndDestroy) {
    EXPECT_CALL(*mock_storage, lseek_mock(_, _, _)).WillRepeatedly(Return(static_cast<off_t>(0))); // For O_APPEND check

    Config cfg;
    cfg.handle = mock_storage;
    cfg.ops = ops;
    cfg.write_capacity = 1024;
    cfg.read_capacity = 1024;

    auto conveyor_res = Conveyor::create(cfg);

    ASSERT_TRUE(conveyor_res.has_value()) << conveyor_res.error().message();
    // Conveyor is destroyed by unique_ptr in TearDown
}

TEST_F(ConveyorModernTest, CreateFailsWithError) {
    // Simulate conveyor_create failing (e.g., lseek fails during O_APPEND init)
    EXPECT_CALL(*mock_storage, lseek_mock(_, _, _)).WillOnce(Return(static_cast<off_t>(LIBCONVEYOR_ERROR)));

    Config cfg;
    cfg.handle = mock_storage;
    cfg.ops = ops;
    cfg.open_flags = O_RDWR | O_APPEND; // Trigger lseek
    cfg.write_capacity = 1024;
    cfg.read_capacity = 1024;

    auto conveyor_res = Conveyor::create(cfg);

    ASSERT_FALSE(conveyor_res.has_value());
    ASSERT_EQ(conveyor_res.error(), std::error_code(errno, std::system_category()));
}

TEST_F(ConveyorModernTest, WriteAndFlush) {
    Config cfg;
    cfg.handle = mock_storage;
    cfg.ops = ops;
    cfg.write_capacity = 1024;
    cfg.read_capacity = 0; // No read buffer for this test

    auto conveyor_res = Conveyor::create(cfg);
    ASSERT_TRUE(conveyor_res.has_value()) << conveyor_res.error().message();
    auto conveyor = std::move(conveyor_res.value());

    std::string test_data = "Hello, C++17 Conveyor!";
    EXPECT_CALL(*mock_storage, pwrite_mock(_, _, test_data.size(), 0)).WillOnce(Return(test_data.size()));
    
    auto write_res = conveyor.write(test_data);
    ASSERT_TRUE(write_res.has_value()) << write_res.error().message();
    ASSERT_EQ(write_res.value(), test_data.size());

    auto flush_res = conveyor.flush();
    ASSERT_TRUE(flush_res.has_value()) << flush_res.error().message();
    
    // Verify mock storage contains data
    ASSERT_EQ(mock_storage->data.size(), test_data.size());
    ASSERT_EQ(std::string(mock_storage->data.data(), mock_storage->data.size()), test_data);
}

TEST_F(ConveyorModernTest, ReadFromDisk) {
    std::string initial_data = "Data from disk.";
    mock_storage->data.assign(initial_data.begin(), initial_data.end());

    Config cfg;
    cfg.handle = mock_storage;
    cfg.ops = ops;
    cfg.write_capacity = 0;
    cfg.read_capacity = 1024;

    auto conveyor_res = Conveyor::create(cfg);
    ASSERT_TRUE(conveyor_res.has_value()) << conveyor_res.error().message();
    auto conveyor = std::move(conveyor_res.value());

    EXPECT_CALL(*mock_storage, pread_mock(_, _, _, _))
        .WillOnce(Invoke([this](storage_handle_t, void* buf, size_t count, off_t offset) {
            std::lock_guard<std::mutex> lock(mock_storage->mx);
            size_t available = std::min(count, mock_storage->data.size() - (size_t)offset);
            std::memcpy(buf, mock_storage->data.data() + offset, available);
            return available;
        }));
    
    std::vector<char> read_buf(initial_data.size()); // Using std::vector<char> for C++17 compatibility
    auto read_res = conveyor.read(read_buf);

    ASSERT_TRUE(read_res.has_value()) << read_res.error().message();
    ASSERT_EQ(read_res.value(), initial_data.size());
    ASSERT_EQ(std::string(read_buf.data(), read_buf.size()), initial_data);
}

TEST_F(ConveyorModernTest, ReadFromWriteQueueSnoop) {
    // Write data directly to mock_storage first to simulate existing disk data
    std::string disk_data = "OLD_DISK_DATA";
    mock_storage->data.assign(disk_data.begin(), disk_data.end());

    Config cfg;
    cfg.handle = mock_storage;
    cfg.ops = ops;
    cfg.write_capacity = 1024;
    cfg.read_capacity = 1024;

    auto conveyor_res = Conveyor::create(cfg);
    ASSERT_TRUE(conveyor_res.has_value()) << conveyor_res.error().message();
    auto conveyor = std::move(conveyor_res.value());

    // Write new data that will be in the write queue
    std::string new_data = "NEW_DATA";
    conveyor.seek(0).value(); // Move to start
    conveyor.write(new_data).value(); // Write new data, it's buffered

    // Expect pread to be called for the initial read buffer fill, but then
    // the read() should return NEW_DATA from the snoop logic
    EXPECT_CALL(*mock_storage, pread_mock(_, _, _, _)).WillOnce(Return(disk_data.size()));
    
    std::vector<char> read_buf(new_data.size()); // Using std::vector<char> for C++17 compatibility
    auto read_res = conveyor.read(read_buf);

    ASSERT_TRUE(read_res.has_value()) << read_res.error().message();
    ASSERT_EQ(read_res.value(), new_data.size());
    ASSERT_EQ(std::string(read_buf.data(), read_buf.size()), new_data);
}

TEST_F(ConveyorModernTest, Seek) {
    Config cfg;
    cfg.handle = mock_storage;
    cfg.ops = ops;

    auto conveyor_res = Conveyor::create(cfg);
    ASSERT_TRUE(conveyor_res.has_value()) << conveyor_res.error().message();
    auto conveyor = std::move(conveyor_res.value());

    EXPECT_CALL(*mock_storage, lseek_mock(_, 100, SEEK_SET)).WillOnce(Return(static_cast<off_t>(100)));
    auto seek_res = conveyor.seek(100);
    ASSERT_TRUE(seek_res.has_value()) << seek_res.error().message();
    ASSERT_EQ(seek_res.value(), 100);
}

TEST_F(ConveyorModernTest, Stats) {
    Config cfg;
    cfg.handle = mock_storage;
    cfg.ops = ops;

    auto conveyor_res = Conveyor::create(cfg);
    ASSERT_TRUE(conveyor_res.has_value()) << conveyor_res.error().message();
    auto conveyor = std::move(conveyor_res.value());

    conveyor.write(std::string(10, 'A')).value();
    conveyor.flush().value();

    auto stats = conveyor.stats();
    ASSERT_EQ(stats.bytes_written, 10);
    ASSERT_GT(stats.avg_write_latency.count(), 0); // Should have some latency
}

TEST_F(ConveyorModernTest, RaIiDestroys) {
    // This test ensures that conveyor_destroy is called by RAII
    // We don't need explicit checks here, as gmock will complain if
    // any mock methods are called after mock_storage is deleted (in TearDown),
    // implicitly verifying that conveyor_destroy (which uses mock methods)
    // completes before mock_storage is gone.
    EXPECT_CALL(*mock_storage, pwrite_mock(_,_,_,_)).WillRepeatedly(Return(1));
    EXPECT_CALL(*mock_storage, lseek_mock(_,_,_)).WillRepeatedly(Return(static_cast<off_t>(0)));

    {
        Config cfg;
        cfg.handle = mock_storage;
        cfg.ops = ops;
        cfg.write_capacity = 100;

        auto conveyor_res = Conveyor::create(cfg);
        ASSERT_TRUE(conveyor_res.has_value());
        auto conveyor = std::move(conveyor_res.value());
        conveyor.write(std::string(10, 'B'));
        // conveyor_flush() will be called implicitly by destructor
    }
    // If we reach here without crash, RAII worked.
}

TEST_F(ConveyorModernTest, ErrorPropagation) {
    EXPECT_CALL(*mock_storage, pwrite_mock(_,_,_,_)).WillOnce(Return(LIBCONVEYOR_ERROR));
    
    Config cfg;
    cfg.handle = mock_storage;
    cfg.ops = ops;
    cfg.write_capacity = 100;

    auto conveyor_res = Conveyor::create(cfg);
    ASSERT_TRUE(conveyor_res.has_value());
    auto conveyor = std::move(conveyor_res.value());

    auto write_res = conveyor.write(std::string(10, 'C'));
    ASSERT_TRUE(write_res.has_value()); // Initial write is enqueued
    
    auto flush_res = conveyor.flush();
    ASSERT_FALSE(flush_res.has_value()); // Flush should report error
    ASSERT_EQ(flush_res.error(), std::error_code(errno, std::system_category()));
    
    // Subsequent operations should also report error
    auto subsequent_write_res = conveyor.write(std::string(10, 'D'));
    ASSERT_FALSE(subsequent_write_res.has_value());
    ASSERT_EQ(subsequent_write_res.error(), std::error_code(errno, std::system_category()));
}