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
         bytes_to_read = std::min((size_t)count, g_mock_storage_data.size() - offset);
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

void test_small_buffer_fragmentation() {
    reset_mock_storage();
    storage_operations_t mock_ops = {mock_pwrite, mock_pread, mock_lseek};
    
    std::string test_data(200, 'x'); // Total data to write
    const size_t write_buffer_size = 50; // Max bytes in queue
    const size_t chunk_size = 10; // Write in 10 byte chunks

    conveyor_t* write_conv = conveyor_create(1, O_WRONLY, &mock_ops, write_buffer_size, 0);
    TEST_ASSERT(write_conv != nullptr, "conveyor_create for write_conv returned nullptr");
    
    ssize_t total_written = 0;
    for(size_t i = 0; i < test_data.length(); i += chunk_size) {
        size_t len = std::min(chunk_size, test_data.length() - i);
        total_written += conveyor_write(write_conv, test_data.c_str() + i, len);
    }
    TEST_ASSERT(total_written == (ssize_t)test_data.length(), "Total bytes written mismatch");
    conveyor_destroy(write_conv);
    TEST_ASSERT(g_mock_storage_data.size() == test_data.length(), "Mock storage size mismatch after fragmentation write");
    TEST_ASSERT(std::string(g_mock_storage_data.data(), g_mock_storage_data.size()) == test_data, "Data mismatch in mock storage after fragmentation write");

    conveyor_t* read_conv = conveyor_create(1, O_RDONLY, &mock_ops, 0, 20); // Read buffer still 20
    TEST_ASSERT(read_conv != nullptr, "conveyor_create for read_conv returned nullptr");
    std::vector<char> read_buffer(test_data.length());
    ssize_t bytes_read = conveyor_read(read_conv, read_buffer.data(), read_buffer.size());
    TEST_ASSERT(bytes_read == (ssize_t)test_data.length(), "Total bytes read mismatch after fragmentation");
    TEST_ASSERT(std::string(read_buffer.data(), read_buffer.size()) == test_data, "Data mismatch after fragmentation read");
    conveyor_destroy(read_conv);
}

void test_multithreaded_writes() {
    reset_mock_storage();
    storage_operations_t mock_ops = {mock_pwrite, mock_pread, mock_lseek};
    conveyor_t* conv = conveyor_create(1, O_WRONLY | O_APPEND, &mock_ops, 1024, 0);
    TEST_ASSERT(conv != nullptr, "conveyor_create returned nullptr");

    std::vector<std::thread> threads;
    const int num_threads = 8;
    const int writes_per_thread = 50;
    
    for (int i = 0; i < num_threads; ++i) {
        threads.emplace_back([i, conv, writes_per_thread]() {
            std::string data_to_write = "Thread" + std::to_string(i) + " writes this data.";
            for (int j = 0; j < writes_per_thread; ++j) {
                conveyor_write(conv, data_to_write.c_str(), data_to_write.length());
            }
        });
    }

    for (auto& t : threads) {
        t.join();
    }

    conveyor_destroy(conv);

    size_t expected_size = 0;
    for (int i=0; i<num_threads; ++i) {
        expected_size += ("Thread" + std::to_string(i) + " writes this data.").length() * writes_per_thread;
    }
    TEST_ASSERT(g_mock_storage_data.size() == expected_size, "Mock storage size mismatch after multithreaded writes");
}

void test_random_seek_stress() {
    reset_mock_storage();
    storage_operations_t mock_ops = {mock_pwrite, mock_pread, mock_lseek};
    conveyor_t* conv = conveyor_create(1, O_RDWR, &mock_ops, 256, 256);
    TEST_ASSERT(conv != nullptr, "conveyor_create returned nullptr");

    const int file_size = 4096;
    std::vector<char> local_copy(file_size, 'A');
    mock_pwrite(1, local_copy.data(), local_copy.size(), 0);

    std::mt19937 rng(std::chrono::high_resolution_clock::now().time_since_epoch().count());
    std::uniform_int_distribution<int> pos_dist(0, file_size - 1);
    std::uniform_int_distribution<int> len_dist(1, 32);
    std::uniform_int_distribution<int> char_dist('B', 'Z');

    const auto start_time = std::chrono::steady_clock::now();
    const std::chrono::seconds timeout(30);

    for (int i = 0; i < 200; ++i) {
        auto now = std::chrono::steady_clock::now();
        TEST_ASSERT(now - start_time < timeout, "Test timed out!");

        off_t seek_pos = pos_dist(rng);
        conveyor_lseek(conv, seek_pos, SEEK_SET);

        int write_len = len_dist(rng);
        std::vector<char> write_data;
        for (int j = 0; j < write_len; ++j) write_data.push_back(static_cast<char>(char_dist(rng)));

        if (seek_pos + write_len > file_size) {
            write_len = file_size - seek_pos;
        }

        if (write_len > 0) {
            conveyor_write(conv, write_data.data(), write_len);
            std::memcpy(local_copy.data() + seek_pos, write_data.data(), write_len);
        }
    }

    conveyor_destroy(conv);

    TEST_ASSERT(g_mock_storage_data == local_copy, "Data mismatch after random seek stress");
}

void test_stats_collection() {
    reset_mock_storage();
    g_simulate_slow_write = true;
    storage_operations_t mock_ops = {mock_pwrite, mock_pread, mock_lseek};
    conveyor_t* conv = conveyor_create(1, O_RDWR, &mock_ops, 50, 50);
    TEST_ASSERT(conv != nullptr, "conveyor_create returned nullptr");

    conveyor_write(conv, "12345", 5);
    conveyor_write(conv, "12345", 5);

    // Ensure all writes are processed before collecting stats
    conveyor_flush(conv);
    
    conveyor_stats_t stats;
    int ret = conveyor_get_stats(conv, &stats);
    TEST_ASSERT(ret == 0, "conveyor_get_stats returned non-zero");
    TEST_ASSERT(stats.bytes_written == 10, "stats.bytes_written mismatch");
    TEST_ASSERT(stats.avg_write_latency_ms > 0, "stats.avg_write_latency_ms not greater than 0");
    TEST_ASSERT(stats.last_error_code == 0, "stats.last_error_code not 0");

    ret = conveyor_get_stats(conv, &stats);
    TEST_ASSERT(ret == 0, "conveyor_get_stats returned non-zero on second call");
    TEST_ASSERT(stats.bytes_written == 0, "stats.bytes_written not 0 after reset");
    TEST_ASSERT(stats.avg_write_latency_ms == 0, "stats.avg_write_latency_ms not 0 after reset");
    
    conveyor_destroy(conv);
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

void test_read_after_write_consistency() {
    reset_mock_storage();
    storage_operations_t mock_ops = {mock_pwrite, mock_pread, mock_lseek};
    conveyor_t* conv = conveyor_create(1, O_RDWR, &mock_ops, 1024, 1024);
    TEST_ASSERT(conv != nullptr, "conveyor_create returned nullptr");

    std::string pattern = "0xDEADBEEF";
    off_t offset = 0;

    // 1. Write a pattern to offset 0
    ssize_t bytes_written = conveyor_write(conv, pattern.c_str(), pattern.length());
    TEST_ASSERT(bytes_written == (ssize_t)pattern.length(), "conveyor_write did not write all bytes");

    // 2. Immediately lseek to 0 and read
    off_t seek_result = conveyor_lseek(conv, offset, SEEK_SET);
    TEST_ASSERT(seek_result == offset, "conveyor_lseek did not seek to correct offset");

    std::vector<char> read_buffer(pattern.length() + 1, '\0');
    ssize_t bytes_read = conveyor_read(conv, read_buffer.data(), pattern.length());

    // 3. Assert: The data read MUST be the pattern
    TEST_ASSERT(bytes_read == (ssize_t)pattern.length(), "conveyor_read did not read all bytes");
    TEST_ASSERT(std::string(read_buffer.data(), bytes_read) == pattern, "Data mismatch after read-after-write");

    conveyor_destroy(conv);
}

void test_ring_buffer_wrap_around() {
    // Directly test the internal RingBuffer struct
    libconveyor::RingBuffer rb(10); // Small capacity
    std::string data1 = "ABCDEFG"; // 7 bytes
    std::string data2 = "HIJKL";   // 5 bytes (wraps around)
    std::string expected_data = "CDEFGHIJKL";

    // Fill buffer partially
    rb.write(data1.c_str(), data1.length());
    TEST_ASSERT(rb.available_data() == 7, "RingBuffer data size mismatch after first write");

    // Force wrap-around write
    rb.write(data2.c_str(), data2.length());
    TEST_ASSERT(rb.available_data() == 10, "RingBuffer data size mismatch after wrap-around write");

    std::vector<char> read_buf(11);
    size_t bytes_read = rb.read(read_buf.data(), 10);
    TEST_ASSERT(bytes_read == 10, "RingBuffer did not read all bytes after wrap-around");
    TEST_ASSERT(std::string(read_buf.data(), bytes_read) == expected_data.substr(0,10), "RingBuffer data mismatch after wrap-around read");
}

void test_unbuffered_write_error_propagation() {
    reset_mock_storage();
    // Use mock_pwrite_fail_once for the pwrite operation
    storage_operations_t mock_ops = {mock_pwrite_fail_once, mock_pread, mock_lseek};

    // Create conveyor with NO write buffer (0 capacity)
    conveyor_t* conv = conveyor_create(1, O_WRONLY, &mock_ops, 0, 0);
    TEST_ASSERT(conv != nullptr, "conveyor_create returned nullptr for unbuffered write test");

    std::string test_data_1 = "First Write - Should Fail";
    std::string test_data_2 = "Second Write - Should Succeed";

    // Attempt the first write - should fail
    ssize_t bytes_written_1 = conveyor_write(conv, test_data_1.c_str(), test_data_1.length());
    TEST_ASSERT(bytes_written_1 == LIBCONVEYOR_ERROR, "First unbuffered conveyor_write should have failed");
    TEST_ASSERT(errno == EIO, "errno should be EIO after failed unbuffered write");
    TEST_ASSERT(g_mock_storage_data.empty(), "Mock storage should be empty after failed write");

    // Clear errno for the next operation
    errno = 0;

    // Attempt the second write - should succeed (due to fail-once logic)
    ssize_t bytes_written_2 = conveyor_write(conv, test_data_2.c_str(), test_data_2.length());
    TEST_ASSERT(bytes_written_2 == (ssize_t)test_data_2.length(), "Second unbuffered conveyor_write should have succeeded");
    TEST_ASSERT(errno == 0, "errno should be 0 after successful unbuffered write");
    TEST_ASSERT(g_mock_storage_data.size() == test_data_2.length(), "Mock storage size mismatch after successful write");
    TEST_ASSERT(std::string(g_mock_storage_data.data(), g_mock_storage_data.size()) == test_data_2, "Data mismatch in mock storage after successful write");

    conveyor_destroy(conv);
}

/*
// Removed test_slow_backend_saturation for now due to complexities in synchronization
void test_slow_backend_saturation() {
    reset_mock_storage();
    g_simulate_slow_write = true; // Make backend writes slow
    static const std::chrono::milliseconds old_simulated_latency = g_simulated_latency; // Save original
    g_simulated_latency = std::chrono::milliseconds(50); // Set to 50ms for this test
    storage_operations_t mock_ops = {mock_pwrite, mock_pread, mock_lseek};

    const size_t write_buffer_capacity = 100; // Large enough buffer
    const size_t item_size = 10;
    std::string item_data(item_size, 'X'); // "XXXXXXXXXX"

    conveyor_t* conv = conveyor_create(1, O_WRONLY, &mock_ops, write_buffer_capacity, 0);
    TEST_ASSERT(conv != nullptr, "conveyor_create returned nullptr");
    std::this_thread::sleep_for(std::chrono::milliseconds(1)); // Allow worker threads to start

    // Fill the buffer to capacity - 1 item
    for (size_t i = 0; i < (write_buffer_capacity / item_size) - 1; ++i) { // 9 items
        ssize_t bytes_written = conveyor_write(conv, item_data.c_str(), item_data.length());
        TEST_ASSERT(bytes_written == (ssize_t)item_data.length(), "Initial write failed to queue item " + std::to_string(i));
    }
    // write_queue_bytes should be 90.

    // Write one more item to make it full (100 bytes). This will be the item that gets "stuck" in mock_pwrite.
    ssize_t bytes_written_last_item = conveyor_write(conv, item_data.c_str(), item_data.length()); // 10th item
    TEST_ASSERT(bytes_written_last_item == (ssize_t)item_data.length(), "Last item write failed to queue");
    // At this point, write_queue is full (100 bytes).
    // writeWorker should pick up an item and call mock_pwrite (which will now block).

    // Wait for writeWorker to enter mock_pwrite and signal that it's waiting
    // This part is implicit - we assume writeWorker is fast enough to pick up an item.
    // The main thread now has a full buffer.

    // Attempt to write one more byte - this should block for the full 30 seconds
    // (conveyor_write's internal timeout) as mock_pwrite is now blocking writeWorker.
    auto start_time = std::chrono::high_resolution_clock::now();
    ssize_t bytes_written_extra = conveyor_write(conv, "B", 1); // Extra byte
    auto end_time = std::chrono::high_resolution_clock::now();
    auto duration = std::chrono::duration_cast<std::chrono::seconds>(end_time - start_time); // Measure in seconds

    // Assertions for the blocking write - it should timeout
    TEST_ASSERT(bytes_written_extra == LIBCONVEYOR_ERROR, "conveyor_write for extra byte should have timed out/failed");
    TEST_ASSERT(errno == ETIMEDOUT, "errno should be ETIMEDOUT");
    TEST_ASSERT(duration >= std::chrono::seconds(30), "conveyor_write should have blocked for at least 30 seconds");
    TEST_ASSERT(duration < std::chrono::seconds(31), "conveyor_write should not block for extremely long (e.g., more than 31 seconds)");

    // Unblock mock_pwrite to allow conveyor_destroy to proceed
    //g_pwrite_block_promise.set_value(); 
    std::this_thread::sleep_for(g_simulated_latency); // Allow writeWorker to finish processing

    conveyor_destroy(conv); // This will flush all pending writes

    // Verify the data that made it to storage
    // Total written should be write_buffer_capacity (100 'X's)
    TEST_ASSERT(g_mock_storage_data.size() == write_buffer_capacity, "Mock storage size mismatch after flush");
    TEST_ASSERT(std::string(g_mock_storage_data.data(), write_buffer_capacity) == std::string(write_buffer_capacity, 'X'), "Data mismatch in mock storage for initial data");
    
    g_simulated_latency = old_simulated_latency; // Restore original latency
}
*/

int main() {
    test_create_destroy();
    test_write_and_flush();
    test_buffered_read();
    test_fast_write_hiding();
    test_fast_read_hiding();
    test_zero_byte_operations();
    test_small_buffer_fragmentation();
    test_multithreaded_writes();
    test_random_seek_stress();
    test_stats_collection();
    test_read_sees_unflushed_write();
    test_read_after_write_consistency();
    test_ring_buffer_wrap_around();
    test_unbuffered_write_error_propagation(); // New test call
    // test_slow_backend_saturation(); // Commented out for now


    if (g_test_failed) {
        std::cerr << "!!! One or more tests FAILED !!!" << std::endl;
        return 1;
    } else {
        std::cout << "All tests passed!" << std::endl;
        return 0;
    }
}