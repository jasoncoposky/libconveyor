#include <gtest/gtest.h>
#include "libconveyor/conveyor.h"
#include "libconveyor/detail/ThreadPool.hpp"
#include <cstring>
#include <vector>
#include <errno.h>

using namespace libconveyor;

// Mock operations for coverage extension
static ssize_t mock_pwrite_success(storage_handle_t h, const void* buf, size_t count, off_t offset) {
    return (ssize_t)count;
}
static ssize_t mock_pread_success(storage_handle_t h, void* buf, size_t count, off_t offset) {
    return (ssize_t)count;
}
static off_t mock_lseek_success(storage_handle_t h, off_t offset, int whence) {
    return offset;
}

TEST(CoverageExtension, ZeroCopyAPI) {
    storage_operations_t ops = { mock_pwrite_success, mock_pread_success, mock_lseek_success };
    conveyor_config_t cfg = {0};
    cfg.ops = ops;
    cfg.initial_write_size = 8192;
    cfg.write_chunk_size = 4096;
    
    conveyor_t* conv = conveyor_create(&cfg);
    ASSERT_NE(conv, nullptr);

    // 1. Test get_buffer + submit_buffer (Map HIT path)
    size_t buffer_size = 0;
    void* buf1 = conveyor_get_buffer(conv, &buffer_size);
    ASSERT_NE(buf1, nullptr);
    std::strcpy((char*)buf1, "MapHitSubmit");
    ssize_t res1 = conveyor_submit_buffer(conv, buf1, 13, 0);
    ASSERT_EQ(res1, 13);
    
    // 2. Test get_buffer + release_buffer (Map HIT path)
    void* buf2 = conveyor_get_buffer(conv, &buffer_size);
    ASSERT_NE(buf2, nullptr);
    conveyor_release_buffer(conv, buf2);

    // 3. Test submit_buffer with manual buffer (Map MISS path)
    void* my_buf;
    posix_memalign(&my_buf, 4096, 4096);
    std::strcpy((char*)my_buf, "MapMissSubmit");
    ssize_t res3 = conveyor_submit_buffer(conv, my_buf, 14, 100);
    ASSERT_EQ(res3, 14);

    conveyor_release_buffer(conv, nullptr);
    
    conveyor_destroy(conv);
}

TEST(CoverageExtension, ErrorPaths) {
    storage_operations_t ops = { mock_pwrite_success, mock_pread_success, mock_lseek_success };
    conveyor_config_t cfg = {0};
    cfg.ops = ops;
    cfg.flags = O_WRONLY;
    cfg.initial_write_size = 100;
    cfg.write_chunk_size = 100;
    
    conveyor_t* conv = conveyor_create(&cfg);
    
    // 1. EMSGSIZE
    std::string large_data(200, 'X');
    ssize_t res = conveyor_write(conv, large_data.c_str(), large_data.length());
    ASSERT_EQ(res, LIBCONVEYOR_ERROR);
    ASSERT_EQ(errno, EMSGSIZE);

    // 2. EINVAL in read (manual creation of invalid state is hard, but we can test 0 initial read size)
    // Actually conveyor_create ensures at least 1 byte if 0 is passed.
    
    conveyor_destroy(conv);
}

TEST(CoverageExtension, ZeroCopyExhaustion) {
    storage_operations_t ops = { mock_pwrite_success, mock_pread_success, mock_lseek_success };
    conveyor_config_t cfg = {0};
    cfg.ops = ops;
    cfg.initial_write_size = 4096;
    cfg.write_chunk_size = 4096;
    
    conveyor_t* conv = conveyor_create(&cfg);
    
    size_t s;
    void* b1 = conveyor_get_buffer(conv, &s);
    ASSERT_NE(b1, nullptr);
    
    // Pool only has 1 segment, so next call should return NULL
    void* b2 = conveyor_get_buffer(conv, &s);
    ASSERT_EQ(b2, nullptr);
    
    conveyor_destroy(conv);
}

TEST(CoverageExtension, ThreadPool) {
    auto pool = ThreadPool::get_shared_instance();
    ASSERT_NE(pool, nullptr);
    
    std::atomic<int> counter{0};
    pool->parallel_for(0, 10, [&](size_t i, size_t end) {
        counter.fetch_add(1);
    });
    // parallel_for is synchronous in citor
    ASSERT_EQ(counter.load(), 10);
    
    pool->shutdown();
    // After shutdown, submit should do nothing
    pool->submit_detached([&]() {
        counter.fetch_add(100);
    });
    std::this_thread::sleep_for(std::chrono::milliseconds(100));
    ASSERT_EQ(counter.load(), 10);
}
