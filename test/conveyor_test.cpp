#include "libconveyor/conveyor.h"
#include <vector>
#include <string>
#include <cstring>
#include <fcntl.h>
#include <cassert>
#include <algorithm>
#include <mutex>
#include <thread>
#include <chrono>
#include <atomic>
#include <random>

// --- Mock Infrastructure ---
static std::vector<char> g_mock_storage_data;
static std::mutex g_mock_storage_mutex;

static std::atomic<bool> g_simulate_slow_write = false;
static std::atomic<bool> g_simulate_slow_read = false;
static const std::chrono::milliseconds g_simulated_latency(50);

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
}

// --- Test Cases ---

void test_create_destroy() {
    reset_mock_storage();
    storage_operations_t mock_ops = {mock_pwrite, mock_pread, mock_lseek};
    conveyor_t* conv = conveyor_create(1, O_RDWR, mock_ops, 1024, 1024);
    assert(conv != nullptr);
    conveyor_destroy(conv);
}

void test_write_and_flush() {
    reset_mock_storage();
    storage_operations_t mock_ops = {mock_pwrite, mock_pread, mock_lseek};
    conveyor_t* conv = conveyor_create(1, O_WRONLY, mock_ops, 1024, 0);
    assert(conv != nullptr);

    std::string test_data = "Hello, Conveyor!";
    ssize_t bytes_written = conveyor_write(conv, test_data.c_str(), test_data.length());
    assert(bytes_written == (ssize_t)test_data.length());

    conveyor_flush(conv);
    conveyor_destroy(conv);

    assert(g_mock_storage_data.size() == test_data.length());
    assert(std::string(g_mock_storage_data.data(), g_mock_storage_data.size()) == test_data);
}

void test_buffered_read() {
    reset_mock_storage();
    storage_operations_t mock_ops = {mock_pwrite, mock_pread, mock_lseek};
    
    std::string test_data = "This is a test of the buffered read functionality.";
    mock_pwrite(1, test_data.c_str(), test_data.length(), 0);

    conveyor_t* conv = conveyor_create(1, O_RDONLY, mock_ops, 0, 1024);
    assert(conv != nullptr);
    
    std::vector<char> read_buffer(test_data.length() + 1, '\0');
    ssize_t bytes_read = conveyor_read(conv, read_buffer.data(), test_data.length());
    
    assert(bytes_read == (ssize_t)test_data.length());
    assert(std::string(read_buffer.data()) == test_data);

    conveyor_destroy(conv);
}

void test_fast_write_hiding() {
    reset_mock_storage();
    g_simulate_slow_write = true;
    storage_operations_t mock_ops = {mock_pwrite, mock_pread, mock_lseek};
    
    conveyor_t* conv = conveyor_create(1, O_WRONLY, mock_ops, 1024, 0);
    assert(conv != nullptr);

    std::string test_data = "This should write instantly.";
    
    auto start_time = std::chrono::high_resolution_clock::now();
    ssize_t bytes_written = conveyor_write(conv, test_data.c_str(), test_data.length());
    auto end_time = std::chrono::high_resolution_clock::now();
    auto duration = std::chrono::duration_cast<std::chrono::milliseconds>(end_time - start_time);

    assert(bytes_written == (ssize_t)test_data.length());
    assert(duration < g_simulated_latency);

    conveyor_destroy(conv);

    assert(g_mock_storage_data.size() == test_data.length());
    assert(std::string(g_mock_storage_data.data(), g_mock_storage_data.size()) == test_data);
}

void test_fast_read_hiding() {
    reset_mock_storage();
    g_simulate_slow_read = true;
    storage_operations_t mock_ops = {mock_pwrite, mock_pread, mock_lseek};

    std::string test_data = "This should be read instantly from cache.";
    mock_pwrite(1, test_data.c_str(), test_data.length(), 0);

    conveyor_t* conv = conveyor_create(1, O_RDONLY, mock_ops, 0, 1024);
    assert(conv != nullptr);
    
    std::this_thread::sleep_for(g_simulated_latency + std::chrono::milliseconds(50));

    std::vector<char> read_buffer(test_data.length() + 1, '\0');
    
    auto start_time = std::chrono::high_resolution_clock::now();
    ssize_t bytes_read = conveyor_read(conv, read_buffer.data(), test_data.length());
    auto end_time = std::chrono::high_resolution_clock::now();
    auto duration = std::chrono::duration_cast<std::chrono::milliseconds>(end_time - start_time);

    assert(bytes_read == (ssize_t)test_data.length());
    assert(duration < g_simulated_latency);
    assert(std::string(read_buffer.data()) == test_data);

    conveyor_destroy(conv);
}


void test_zero_byte_operations() {
    reset_mock_storage();
    storage_operations_t mock_ops = {mock_pwrite, mock_pread, mock_lseek};
    conveyor_t* conv = conveyor_create(1, O_RDWR, mock_ops, 1024, 1024);
    assert(conv != nullptr);
    
    ssize_t bytes_written = conveyor_write(conv, "should not be written", 0);
    assert(bytes_written == 0);

    ssize_t bytes_read = conveyor_read(conv, nullptr, 0);
    assert(bytes_read == 0);

    conveyor_destroy(conv);
    assert(g_mock_storage_data.empty());
}

void test_small_buffer_fragmentation() {
    reset_mock_storage();
    storage_operations_t mock_ops = {mock_pwrite, mock_pread, mock_lseek};
    
    std::string test_data(200, 'x'); // Total data to write
    const size_t write_buffer_size = 50; // Max bytes in queue
    const size_t chunk_size = 10; // Write in 10 byte chunks

    conveyor_t* write_conv = conveyor_create(1, O_WRONLY, mock_ops, write_buffer_size, 0);
    assert(write_conv != nullptr);
    
    ssize_t total_written = 0;
    for(size_t i = 0; i < test_data.length(); i += chunk_size) {
        size_t len = std::min(chunk_size, test_data.length() - i);
        total_written += conveyor_write(write_conv, test_data.c_str() + i, len);
    }
    assert(total_written == (ssize_t)test_data.length());
    conveyor_destroy(write_conv);
    assert(g_mock_storage_data.size() == test_data.length());
    assert(std::string(g_mock_storage_data.data(), g_mock_storage_data.size()) == test_data);

    conveyor_t* read_conv = conveyor_create(1, O_RDONLY, mock_ops, 0, 20); // Read buffer still 20
    assert(read_conv != nullptr);
    std::vector<char> read_buffer(test_data.length());
    ssize_t bytes_read = conveyor_read(read_conv, read_buffer.data(), read_buffer.size());
    assert(bytes_read == (ssize_t)test_data.length());
    assert(std::string(read_buffer.data(), read_buffer.size()) == test_data);
    conveyor_destroy(read_conv);
}

void test_multithreaded_writes() {
    reset_mock_storage();
    storage_operations_t mock_ops = {mock_pwrite, mock_pread, mock_lseek};
    conveyor_t* conv = conveyor_create(1, O_WRONLY | O_APPEND, mock_ops, 1024, 0);
    assert(conv != nullptr);

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
    assert(g_mock_storage_data.size() == expected_size);
}

void test_random_seek_stress() {
    reset_mock_storage();
    storage_operations_t mock_ops = {mock_pwrite, mock_pread, mock_lseek};
    conveyor_t* conv = conveyor_create(1, O_RDWR, mock_ops, 256, 256);
    assert(conv != nullptr);

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
        assert(now - start_time < timeout && "Test timed out!");

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

    assert(g_mock_storage_data == local_copy);
}

void test_stats_collection() {
    reset_mock_storage();
    g_simulate_slow_write = true;
    storage_operations_t mock_ops = {mock_pwrite, mock_pread, mock_lseek};
    conveyor_t* conv = conveyor_create(1, O_RDWR, mock_ops, 50, 50);
    assert(conv != nullptr);

    conveyor_write(conv, "12345", 5);
    conveyor_write(conv, "12345", 5);

    std::this_thread::sleep_for(g_simulated_latency + std::chrono::milliseconds(20));

    conveyor_stats_t stats;
    int ret = conveyor_get_stats(conv, &stats);
    assert(ret == 0);
    assert(stats.bytes_written == 10);
    assert(stats.avg_write_latency_ms > 0);
    assert(stats.last_error_code == 0);

    ret = conveyor_get_stats(conv, &stats);
    assert(ret == 0);
    assert(stats.bytes_written == 0);
    assert(stats.avg_write_latency_ms == 0);
    
    conveyor_destroy(conv);
}

void test_read_sees_unflushed_write() {
    reset_mock_storage();
    g_simulate_slow_write = true; // Ensure writeWorker is slow and writes stay in queue
    storage_operations_t mock_ops = {mock_pwrite, mock_pread, mock_lseek};
    conveyor_t* conv = conveyor_create(1, O_RDWR, mock_ops, 100, 100); // Small buffers
    assert(conv != nullptr);

    std::string test_data = "ABCDE";
    off_t write_offset = 0;
    
    // Write data, it should go to the queue
    ssize_t bytes_written = conveyor_write(conv, test_data.c_str(), test_data.length());
    assert(bytes_written == (ssize_t)test_data.length());

    // Seek to the beginning of the written data
    conveyor_lseek(conv, write_offset, SEEK_SET);

    // Try to read the data back
    std::vector<char> read_buffer(test_data.length() + 1, '\0');
    ssize_t bytes_read = conveyor_read(conv, read_buffer.data(), test_data.length());
    
    assert(bytes_read == (ssize_t)test_data.length());
    assert(std::string(read_buffer.data(), bytes_read) == test_data);

    conveyor_destroy(conv);
}

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
    test_read_sees_unflushed_write(); // New test added here

    return 0;
}