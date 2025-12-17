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
    g_simulate_slow_write = true; // Re-enable slow writes to test original condition
    storage_operations_t mock_ops = {mock_pwrite, mock_pread, mock_lseek};
    conveyor_t* conv = conveyor_create(1, O_RDWR, &mock_ops, 100, 100); // Small buffers
    TEST_ASSERT(conv != nullptr, "conveyor_create returned nullptr");

    std::string test_data = "ABCDE";
    off_t write_offset = 0;
    
    // Write data, it should go to the queue
    ssize_t bytes_written = conveyor_write(conv, test_data.c_str(), test_data.length());
    TEST_ASSERT(bytes_written == (ssize_t)test_data.length(), "conveyor_write did not write all bytes. Expected " + std::to_string(test_data.length()) + ", Got " + std::to_string(bytes_written));

    // Seek to the beginning of the written data
    conveyor_lseek(conv, write_offset, SEEK_SET);

    // Try to read the data back
    std::vector<char> read_buffer(test_data.length() + 1, '\0');
    ssize_t bytes_read = conveyor_read(conv, read_buffer.data(), test_data.length());
    
    std::string read_str(read_buffer.data(), bytes_read > 0 ? bytes_read : 0);
    TEST_ASSERT(bytes_read == (ssize_t)test_data.length(), "conveyor_read did not read all bytes. Expected " + std::to_string(test_data.length()) + ", Got " + std::to_string(bytes_read));
    TEST_ASSERT(read_str == test_data, "Data mismatch after read sees unflushed write. Expected '" + test_data + "', Got '" + read_str + "'");

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


int main(int argc, char **argv) {
    test_create_destroy();
    test_write_and_flush();
    test_buffered_read();
    test_fast_write_hiding();
    test_fast_read_hiding();
    test_zero_byte_operations();
    test_read_sees_unflushed_write();
    test_slow_backend_saturation();

    if (g_test_failed) {
        std::cerr << "!!! One or more tests FAILED !!!" << std::endl;
        return 1;
    } else {
        std::cout << "All tests passed!" << std::endl;
        return 0;
    }
}