#include <gtest/gtest.h>
#include "libconveyor/conveyor.h"
#include <fcntl.h>
#include <cerrno>

// --- Mock Storage Backend ---
class MockStorage {
public:
    static ssize_t pwrite_callback(storage_handle_t, const void*, size_t count, off_t) {
        return count;
    }

    static ssize_t pread_callback(storage_handle_t, void*, size_t count, off_t) {
        return count;
    }

    static off_t lseek_callback(storage_handle_t, off_t offset, int) {
        return offset;
    }
    
    storage_operations_t get_ops() {
        return { pwrite_callback, pread_callback, lseek_callback };
    }
};

// --- Test Fixture ---
class ConveyorIllegalOpTest : public ::testing::Test {
protected:
    MockStorage* mock;
    conveyor_t* conv;
    
    void SetUp() override {
        mock = new MockStorage();
        conv = nullptr;
    }

    void TearDown() override {
        if (conv) conveyor_destroy(conv);
        delete mock;
    }
};

TEST_F(ConveyorIllegalOpTest, WriteOnReadOnly) {
    auto ops = mock->get_ops();
    conv = conveyor_create(mock, O_RDONLY, &ops, 0, 4096);
    ASSERT_NE(conv, nullptr);

    errno = 0;
    ssize_t res = conveyor_write(conv, "test", 4);
    
    ASSERT_EQ(res, -1);
    ASSERT_EQ(errno, EBADF);
}

TEST_F(ConveyorIllegalOpTest, ReadOnWriteOnly) {
    auto ops = mock->get_ops();
    conv = conveyor_create(mock, O_WRONLY, &ops, 4096, 0);
    ASSERT_NE(conv, nullptr);

    char buf[10];
    errno = 0;
    ssize_t res = conveyor_read(conv, buf, 10);
    
    ASSERT_EQ(res, -1);
    ASSERT_EQ(errno, EBADF);
}
