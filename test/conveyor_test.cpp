#include "libconveyor/conveyor.h"
#include <vector>
#include <string>
#include <cstring>
#include <fcntl.h>
#include <cassert> // Keep for now for reference, will remove
#include <algorithm>
#include <mutex>
#include <thread>
#include <chrono>
#include <atomic>
#include <random>
#include <iostream> // Explicitly include for std::cerr and std::cout
#include <future> // For std::promise and std::future
#include "libconveyor/detail/ring_buffer.h"


// Global flag to track if any test has failed
static bool g_test_failed = false;

// Custom assertion macro
#define TEST_ASSERT(condition, message) \
    do { \
        if (!(condition)) { \
            std::cerr << "Test Failed: " << message << std::endl; \
            std::cerr << "  Condition: " << #condition << std::endl; \
            std::cerr << "  File: " << __FILE__ << ", Line: " << __LINE__ << std::endl; \
            g_test_failed = true; \
            /* In a real test framework, you might throw an exception here or jump to a cleanup point */ \
        } \
    } while(0)

// --- Mock Infrastructure ---
static std::vector<char> g_mock_storage_data;
static std::mutex g_mock_storage_mutex;

static std::atomic<bool> g_simulate_slow_write = false;
static std::atomic<bool> g_simulate_slow_read = false;
static std::chrono::milliseconds g_simulated_latency(5);

static ssize_t mock_pwrite(storage_handle_t, const void* buf, size_t count, off_t offset) {
    if (g_simulate_slow_write) {
        std::this_thread::sleep_for(g_simulated_latency);
    }
    std::lock_guard<std::mutex> lock(g_mock_storage_mutex);
    if (offset + count > g_mock_storage_data.size()) {
        g_mock_storage_data.resize(offset + count);
    }
    std::memcpy(g_mock_storage_data.data() + offset, buf, count);
    return count;
}

static std::atomic<int> g_pwrite_fail_once_counter(0);

static ssize_t mock_pwrite_fail_once(storage_handle_t handle, const void* buf, size_t count, off_t offset) {
    if (g_pwrite_fail_once_counter.fetch_add(1) == 0) {
        errno = EIO; // Simulate I/O error
        return LIBCONVEYOR_ERROR;
    }
    // After the first failure, delegate to mock_pwrite's normal behavior
    return mock_pwrite(handle, buf, count, offset);
}

static ssize_t mock_pread(storage_handle_t, void* buf, size_t count, off_t offset) {
    if (g_simulate_slow_read) {
        std::this_thread::sleep_for(g_simulated_latency);
    }
    std::lock_guard<std::mutex> lock(g_mock_storage_mutex);
    size_t bytes_to_read = 0;
    if (offset >= 0 && offset < (off_t)g_mock_storage_data.size()) {
         bytes_to_read = std::min((size_t)count, g_mock_storage_data.size() - (size_t)offset);
    }
    
    if (bytes_to_read > 0) {
        std::memcpy(buf, g_mock_storage_data.data() + offset, bytes_to_read);
    }
    return bytes_to_read;
}

static off_t mock_lseek(storage_handle_t, off_t offset, int whence) {
    std::lock_guard<std::mutex> lock(g_mock_storage_mutex);
    off_t new_pos = LIBCONVEYOR_ERROR;
    if (whence == SEEK_SET) new_pos = offset;
    else if (whence == SEEK_END) new_pos = static_cast<off_t>(g_mock_storage_data.size()) + offset;
    
    if (new_pos >= 0) return new_pos;
    return LIBCONVEYOR_ERROR;
}

void reset_mock_storage() {
    std::lock_guard<std::mutex> lock(g_mock_storage_mutex);
    g_mock_storage_data.clear();
    g_simulate_slow_write = false;
    g_simulate_slow_read = false;
    g_pwrite_fail_once_counter = 0; // Reset counter for fail-once mock
}

// --- Test Cases ---

void test_create_destroy() {
    reset_mock_storage();
    storage_operations_t mock_ops = {mock_pwrite, mock_pread, mock_lseek};
    conveyor_t* conv = conveyor_create(1, O_RDWR, &mock_ops, 1024, 1024);
    TEST_ASSERT(conv != nullptr, "conveyor_create returned nullptr");
    conveyor_destroy(conv);
}

void test_write_and_flush() {
    reset_mock_storage();
    storage_operations_t mock_ops = {mock_pwrite, mock_pread, mock_lseek};
    conveyor_t* conv = conveyor_create(1, O_WRONLY, &mock_ops, 1024, 0);
    TEST_ASSERT(conv != nullptr, "conveyor_create returned nullptr");

    std::string test_data = "Hello, Conveyor!";
    ssize_t bytes_written = conveyor_write(conv, test_data.c_str(), test_data.length());
    TEST_ASSERT(bytes_written == (ssize_t)test_data.length(), "conveyor_write did not write all bytes");

    conveyor_flush(conv);
    conveyor_destroy(conv);

    TEST_ASSERT(g_mock_storage_data.size() == test_data.length(), "Mock storage size mismatch after flush");
    TEST_ASSERT(std::string(g_mock_storage_data.data(), g_mock_storage_data.size()) == test_data, "Data mismatch in mock storage after flush");
}

void test_buffered_read() {
    reset_mock_storage();
    storage_operations_t mock_ops = {mock_pwrite, mock_pread, mock_lseek};
    
    std::string test_data = "This is a test of the buffered read functionality.";
    mock_pwrite(1, test_data.c_str(), test_data.length(), 0);

    conveyor_t* conv = conveyor_create(1, O_RDONLY, &mock_ops, 0, 1024);
    TEST_ASSERT(conv != nullptr, "conveyor_create returned nullptr");
    
    std::vector<char> read_buffer(test_data.length() + 1, '\0');
    ssize_t bytes_read = conveyor_read(conv, read_buffer.data(), test_data.length());
    
    TEST_ASSERT(bytes_read == (ssize_t)test_data.length(), "conveyor_read did not read all bytes");
    TEST_ASSERT(std::string(read_buffer.data()) == test_data, "Data mismatch after buffered read");

    conveyor_destroy(conv);
}

void test_fast_write_hiding() {
    reset_mock_storage();
    g_simulate_slow_write = true;
    storage_operations_t mock_ops = {mock_pwrite, mock_pread, mock_lseek};
    
    conveyor_t* conv = conveyor_create(1, O_WRONLY, &mock_ops, 1024, 0);
    TEST_ASSERT(conv != nullptr, "conveyor_create returned nullptr");

    std::string test_data = "This should write instantly.";
    
    auto start_time = std::chrono::high_resolution_clock::now();
    ssize_t bytes_written = conveyor_write(conv, test_data.c_str(), test_data.length());
    auto end_time = std::chrono::high_resolution_clock::now();
    auto duration = std::chrono::duration_cast<std::chrono::milliseconds>(end_time - start_time);

    TEST_ASSERT(bytes_written == (ssize_t)test_data.length(), "conveyor_write did not write all bytes for fast hiding test");
    TEST_ASSERT(duration < std::chrono::milliseconds(10), "conveyor_write took too long for fast hiding");

    conveyor_destroy(conv);

    TEST_ASSERT(g_mock_storage_data.size() == test_data.length(), "Mock storage size mismatch for fast hiding test");
    TEST_ASSERT(std::string(g_mock_storage_data.data(), g_mock_storage_data.size()) == test_data, "Data mismatch for fast hiding test");
}

void test_fast_read_hiding() {
    reset_mock_storage();
    g_simulate_slow_read = true;
    storage_operations_t mock_ops = {mock_pwrite, mock_pread, mock_lseek};

    std::string test_data = "This should be read instantly from cache.";
    mock_pwrite(1, test_data.c_str(), test_data.length(), 0);

    conveyor_t* conv = conveyor_create(1, O_RDONLY, &mock_ops, 0, 1024);
    TEST_ASSERT(conv != nullptr, "conveyor_create returned nullptr");
    
    std::this_thread::sleep_for(g_simulated_latency + std::chrono::milliseconds(50)); // Give readWorker time to pre-fill

    std::vector<char> read_buffer(test_data.length() + 1, '\0');
    
    auto start_time = std::chrono::high_resolution_clock::now();
    ssize_t bytes_read = conveyor_read(conv, read_buffer.data(), test_data.length());
    auto end_time = std::chrono::high_resolution_clock::now();
    auto duration = std::chrono::duration_cast<std::chrono::milliseconds>(end_time - start_time);

    TEST_ASSERT(bytes_read == (ssize_t)test_data.length(), "conveyor_read did not read all bytes for fast hiding test");
    TEST_ASSERT(duration < std::chrono::milliseconds(10), "conveyor_read took too long for fast hiding");
    TEST_ASSERT(std::string(read_buffer.data()) == test_data, "Data mismatch for fast hiding test");

    conveyor_destroy(conv);
}


void test_zero_byte_operations() {
    reset_mock_storage();
    storage_operations_t mock_ops = {mock_pwrite, mock_pread, mock_lseek};
    conveyor_t* conv = conveyor_create(1, O_RDWR, &mock_ops, 1024, 1024);
    TEST_ASSERT(conv != nullptr, "conveyor_create returned nullptr");
    
    ssize_t bytes_written = conveyor_write(conv, "should not be written", 0);
    TEST_ASSERT(bytes_written == 0, "conveyor_write with 0 bytes did not return 0");

    ssize_t bytes_read = conveyor_read(conv, nullptr, 0);
    TEST_ASSERT(bytes_read == 0, "conveyor_read with 0 bytes did not return 0");

    conveyor_destroy(conv);
    TEST_ASSERT(g_mock_storage_data.empty(), "Mock storage not empty after zero-byte operations");
}

void test_read_sees_unflushed_write() {
    reset_mock_storage();
    g_simulate_slow_write = true;
    storage_operations_t mock_ops = {mock_pwrite, mock_pread, mock_lseek};
    conveyor_t* conv = conveyor_create(1, O_RDWR, &mock_ops, 100, 100);
    TEST_ASSERT(conv != nullptr, "conveyor_create returned nullptr");

    std::string test_data = "ABCDE";
    
    ssize_t bytes_written = conveyor_write(conv, test_data.c_str(), test_data.length());
    TEST_ASSERT(bytes_written == (ssize_t)test_data.length(), "conveyor_write did not write all bytes.");

    // Seek to the beginning. This will FLUSH the write buffer.
    off_t seek_res = conveyor_lseek(conv, 0, SEEK_SET);
    TEST_ASSERT(seek_res == 0, "lseek failed");

    // The data is now in mock_storage. The read buffer should be invalidated and refilled.
    std::vector<char> read_buffer(test_data.length() + 1, '\0');
    ssize_t bytes_read = conveyor_read(conv, read_buffer.data(), test_data.length());
    
    std::string read_str(read_buffer.data(), bytes_read > 0 ? bytes_read : 0);
    TEST_ASSERT(bytes_read == (ssize_t)test_data.length(), "conveyor_read did not read all bytes. Expected " + std::to_string(test_data.length()) + ", Got " + std::to_string(bytes_read));
    TEST_ASSERT(read_str == test_data, "Data mismatch. Expected '" + test_data + "', Got '" + read_str + "'");

    conveyor_destroy(conv);
}

static std::promise<void> g_pwrite_block_promise;
static std::future<void> g_pwrite_block_future;

static ssize_t mock_pwrite_block(storage_handle_t, const void* buf, size_t count, off_t offset) {
    g_pwrite_block_future.wait(); // This will block until the promise is set
    return mock_pwrite(0, buf, count, offset); // Delegate to original mock pwrite after unblocking
}

void test_slow_backend_saturation() {
    reset_mock_storage();
    // Re-initialize promise and future for each test run
    g_pwrite_block_promise = std::promise<void>();
    g_pwrite_block_future = g_pwrite_block_promise.get_future();

    storage_operations_t mock_ops = {mock_pwrite_block, mock_pread, mock_lseek};

    const size_t write_buffer_capacity = 1024;
    const size_t item_size = 100;
    std::string item_data(item_size, 'X');

    conveyor_t* conv = conveyor_create(1, O_WRONLY, &mock_ops, write_buffer_capacity, 0);
    TEST_ASSERT(conv != nullptr, "conveyor_create returned nullptr");
    std::this_thread::sleep_for(std::chrono::milliseconds(100)); // Give writeWorker a chance to start

    // The first write will go to the worker and block in mock_pwrite_block
    conveyor_write(conv, item_data.c_str(), item_data.length());

    // Fill the rest of the buffer
    const size_t num_items_to_fill = (write_buffer_capacity / item_size);
    for (size_t i = 1; i < num_items_to_fill; ++i) {
        ssize_t bytes_written = conveyor_write(conv, item_data.c_str(), item_data.length());
        TEST_ASSERT(bytes_written == (ssize_t)item_data.length(), "Failed to write item " + std::to_string(i) + " to fill the buffer.");
    }

    // The next write should block and time out because the first write is blocked indefinitely
    auto start = std::chrono::high_resolution_clock::now();
    ssize_t final_write = conveyor_write(conv, item_data.c_str(), item_data.length());
    auto end = std::chrono::high_resolution_clock::now();
    auto duration = std::chrono::duration_cast<std::chrono::seconds>(end - start);

    // Assert the timeout behavior
    TEST_ASSERT(final_write == LIBCONVEYOR_ERROR, "Final write should have failed due to timeout");
    TEST_ASSERT(errno == ETIMEDOUT, "Errno should be ETIMEDOUT");
    TEST_ASSERT(duration.count() >= 29 && duration.count() < 31, "Write should have blocked for ~30 seconds");

    // Unblock the pwrite so the conveyor can be destroyed cleanly
    g_pwrite_block_promise.set_value();
    conveyor_destroy(conv);
}


static std::atomic<int> g_pwrite_alternating_counter(0);
static ssize_t mock_pwrite_alternating(storage_handle_t, const void* buf, size_t count, off_t offset) {
    g_pwrite_alternating_counter++;
    std::string data_to_write = "version" + std::to_string(g_pwrite_alternating_counter);
    if (data_to_write.length() > count) {
        data_to_write = data_to_write.substr(0, count);
    } else {
        data_to_write.resize(count, '\0');
    }
    return mock_pwrite(0, data_to_write.c_str(), count, offset);
}

static std::atomic<int> g_pread_alternating_counter(0);
static ssize_t mock_pread_alternating(storage_handle_t, void* buf, size_t count, off_t offset) {
    g_pread_alternating_counter++;
    std::string data_to_write = "version" + std::to_string(g_pread_alternating_counter);
    data_to_write.resize(count, '\0');
    std::memcpy(buf, data_to_write.c_str(), count);
    return count;
}

void test_lseek_invalidation() {
    reset_mock_storage();
    g_pread_alternating_counter = 0;
    storage_operations_t mock_ops = {mock_pwrite, mock_pread_alternating, mock_lseek};

    conveyor_t* conv = conveyor_create(1, O_RDONLY, &mock_ops, 0, 1024);
    TEST_ASSERT(conv != nullptr, "conveyor_create returned nullptr");

    // 1. Read the first version of the data
    char read_buf[20] = {0};
    conveyor_read(conv, read_buf, 8);
    TEST_ASSERT(std::string(read_buf) == "version1", "First read should be version1");

    // 2. Seek, which should invalidate the buffer, and read again
    memset(read_buf, 0, sizeof(read_buf));
    conveyor_lseek(conv, 0, SEEK_SET);
    conveyor_read(conv, read_buf, 8);
    TEST_ASSERT(std::string(read_buf) == "version2", "Second read should be version2");

    conveyor_destroy(conv);
}


static std::atomic<int> g_pwrite_successful_writes_counter(0);
static int g_pwrite_fail_after_n = 0;

static ssize_t mock_pwrite_fail_after_n_writes(storage_handle_t handle, const void* buf, size_t count, off_t offset) {
    if (g_pwrite_fail_after_n > 0 && g_pwrite_successful_writes_counter.load() >= g_pwrite_fail_after_n) {
        errno = EIO; // Simulate I/O error
        return LIBCONVEYOR_ERROR;
    }
    g_pwrite_successful_writes_counter++;
    return mock_pwrite(handle, buf, count, offset);
}

void test_sticky_error_propagation() {
    reset_mock_storage();
    g_pwrite_successful_writes_counter = 0;
    g_pwrite_fail_after_n = 5; // Fail after 5 successful writes

    storage_operations_t mock_ops = {mock_pwrite_fail_after_n_writes, mock_pread, mock_lseek};
    conveyor_t* conv = conveyor_create(1, O_WRONLY, &mock_ops, 1024, 0);
    TEST_ASSERT(conv != nullptr, "conveyor_create returned nullptr");
    
    std::string test_data = "ABCDE"; // 5 bytes

    // Perform 5 successful writes
    for (int i = 0; i < 5; ++i) {
        ssize_t bytes_written = conveyor_write(conv, test_data.c_str(), test_data.length());
        TEST_ASSERT(bytes_written == (ssize_t)test_data.length(), "Write " + std::to_string(i+1) + " should succeed");
    }

    // The 6th write will fail asynchronously in the background
    ssize_t bytes_written_6th = conveyor_write(conv, test_data.c_str(), test_data.length());
    TEST_ASSERT(bytes_written_6th == (ssize_t)test_data.length(), "6th write should be enqueued successfully");

    // Perform subsequent writes (7th, 8th). These should also be enqueued
    ssize_t bytes_written_7th = conveyor_write(conv, test_data.c_str(), test_data.length());
    TEST_ASSERT(bytes_written_7th == (ssize_t)test_data.length(), "7th write should be enqueued successfully");
    ssize_t bytes_written_8th = conveyor_write(conv, test_data.c_str(), test_data.length());
    TEST_ASSERT(bytes_written_8th == (ssize_t)test_data.length(), "8th write should be enqueued successfully");

    // Flush the conveyor, expect it to return error due to the 6th write's failure
    int flush_result = conveyor_flush(conv);
    TEST_ASSERT(flush_result == LIBCONVEYOR_ERROR, "conveyor_flush should return an error");
    TEST_ASSERT(errno == EIO, "errno should be EIO after flush");

    // Verify that the library enters a "safe state"
    errno = 0; // Clear errno
    ssize_t bytes_written_after_failure = conveyor_write(conv, test_data.c_str(), test_data.length());
    TEST_ASSERT(bytes_written_after_failure == LIBCONVEYOR_ERROR, "Write after failure should immediately return error");
    TEST_ASSERT(errno == EIO, "errno should still be EIO for subsequent writes");

    // Verify stats
    conveyor_stats_t stats;
    conveyor_get_stats(conv, &stats);
    TEST_ASSERT(stats.last_error_code == EIO, "last_error_code in stats should be EIO");

    conveyor_destroy(conv);
}


void test_write_larger_than_buffer() {
    reset_mock_storage();
    storage_operations_t mock_ops = {mock_pwrite, mock_pread, mock_lseek};
    
    conveyor_t* conv = conveyor_create(1, O_WRONLY, &mock_ops, 100, 0); // 100 byte buffer
    TEST_ASSERT(conv != nullptr, "conveyor_create returned nullptr");

    std::string large_data(200, 'X'); // 200 bytes
    ssize_t bytes_written = conveyor_write(conv, large_data.c_str(), large_data.length());

    TEST_ASSERT(bytes_written == LIBCONVEYOR_ERROR, "Write larger than buffer should fail");
    TEST_ASSERT(errno == EMSGSIZE, "errno should be EMSGSIZE for write larger than buffer");

    conveyor_destroy(conv);
}

void test_o_append_mode() {
    reset_mock_storage();
    storage_operations_t mock_ops = {mock_pwrite, mock_pread, mock_lseek};
    
    // Write some initial data to the mock storage
    std::string initial_data = "Initial data. ";
    mock_pwrite(1, initial_data.c_str(), initial_data.length(), 0);

    conveyor_t* conv = conveyor_create(1, O_WRONLY | O_APPEND, &mock_ops, 1024, 0);
    TEST_ASSERT(conv != nullptr, "conveyor_create returned nullptr for O_APPEND");

    std::string append_data1 = "Appended data 1. ";
    conveyor_write(conv, append_data1.c_str(), append_data1.length());

    std::string append_data2 = "Appended data 2.";
    conveyor_write(conv, append_data2.c_str(), append_data2.length());

    conveyor_flush(conv);
    conveyor_destroy(conv);

    std::string expected_data = initial_data + append_data1 + append_data2;
    TEST_ASSERT(g_mock_storage_data.size() == expected_data.length(), "O_APPEND: Mock storage size mismatch");
    TEST_ASSERT(std::string(g_mock_storage_data.data(), g_mock_storage_data.size()) == expected_data, "O_APPEND: Data mismatch in mock storage");
}

void test_clear_error() {
    reset_mock_storage();
    g_pwrite_fail_once_counter = 0;
    storage_operations_t mock_ops = {mock_pwrite_fail_once, mock_pread, mock_lseek};
    conveyor_t* conv = conveyor_create(1, O_WRONLY, &mock_ops, 1024, 0);
    TEST_ASSERT(conv != nullptr, "conveyor_create returned nullptr");

    std::string test_data = "some data";
    
    // This first write will fail and set the sticky error
    ssize_t bytes_written_fail = conveyor_write(conv, test_data.c_str(), test_data.length());
    // This write may or may not fail immediately depending on whether the worker has run. 
    // We need to flush to be sure the error is propagated.
    conveyor_flush(conv);

    ssize_t bytes_written_after_fail = conveyor_write(conv, test_data.c_str(), test_data.length());
    TEST_ASSERT(bytes_written_after_fail == LIBCONVEYOR_ERROR, "Write after failure should fail");
    TEST_ASSERT(errno == EIO, "errno should be EIO after failure");

    // Clear the error
    int clear_result = conveyor_clear_error(conv);
    TEST_ASSERT(clear_result == 0, "conveyor_clear_error should succeed");

    // This write should now succeed
    ssize_t bytes_written_after_clear = conveyor_write(conv, test_data.c_str(), test_data.length());
    TEST_ASSERT(bytes_written_after_clear == (ssize_t)test_data.length(), "Write after clear should succeed");

    conveyor_destroy(conv);
}

static std::promise<void> g_pread_block_promise;
static std::future<void> g_pread_block_future;

static ssize_t mock_pread_block(storage_handle_t, void* buf, size_t count, off_t offset) {
    g_pread_block_future.wait(); // This will block until the promise is set
    return mock_pread(0, buf, count, offset); // Delegate to original mock pread after unblocking
}

void test_destructor_teardown_race() {
    for (int i = 0; i < 10000; ++i) {
        reset_mock_storage();
        g_pread_block_promise = std::promise<void>();
        g_pread_block_future = g_pread_block_promise.get_future();
        
        std::promise<void> read_thread_started_promise;
        std::future<void> read_thread_started_future = read_thread_started_promise.get_future();

        storage_operations_t mock_ops = {mock_pwrite, mock_pread_block, mock_lseek};

        conveyor_t* conv = conveyor_create(1, O_RDONLY, &mock_ops, 0, 4096);
        TEST_ASSERT(conv != nullptr, "conveyor_create returned nullptr");

        std::thread read_thread([&]() {
            char read_buf[10];
            read_thread_started_promise.set_value();
            conveyor_read(conv, read_buf, 10);
        });

        read_thread_started_future.wait();
        // Unblock the pread so the worker thread can finish
        g_pread_block_promise.set_value();
        
        conveyor_stop(conv);
        read_thread.join();
        
        // Immediately destroy the conveyor
        conveyor_destroy(conv);
    }
}


int main(int argc, char **argv) {
    test_create_destroy();
    test_write_and_flush();
    test_buffered_read();
    test_fast_write_hiding();
    test_fast_read_hiding();
    test_zero_byte_operations();
    test_write_larger_than_buffer();
    test_read_sees_unflushed_write();
    test_slow_backend_saturation();
    test_lseek_invalidation();
    test_o_append_mode();
    test_sticky_error_propagation();
    test_clear_error();
    test_destructor_teardown_race();

    if (g_test_failed) {
        std::cerr << "!!! One or more tests FAILED !!!" << std::endl;
        return 1;
    } else {
        std::cout << "All tests passed!" << std::endl;
        return 0;
    }
}