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

// Mock storage operations for testing
static std::vector<char> g_mock_storage_data;
static off_t g_mock_storage_pos = 0;
static std::mutex g_mock_storage_mutex;

static ssize_t mock_write(storage_handle_t /*handle*/, const void* buf, size_t count) {
    std::lock_guard<std::mutex> lock(g_mock_storage_mutex);
    if (g_mock_storage_pos + count > g_mock_storage_data.size()) {
        g_mock_storage_data.resize(g_mock_storage_pos + count);
    }
    std::memcpy(g_mock_storage_data.data() + g_mock_storage_pos, buf, count);
    g_mock_storage_pos += static_cast<off_t>(count);
    return count;
}

static ssize_t mock_read(storage_handle_t /*handle*/, void* buf, size_t count) {
    std::lock_guard<std::mutex> lock(g_mock_storage_mutex);
    size_t bytes_to_read = 0;
    if (g_mock_storage_pos < (off_t)g_mock_storage_data.size()) {
         bytes_to_read = std::min((size_t)count, g_mock_storage_data.size() - g_mock_storage_pos);
    }
    
    if (bytes_to_read > 0) {
        std::memcpy(buf, g_mock_storage_data.data() + g_mock_storage_pos, bytes_to_read);
        g_mock_storage_pos += static_cast<off_t>(bytes_to_read);
    }
    return bytes_to_read;
}

static off_t mock_lseek(storage_handle_t /*handle*/, off_t offset, int whence) {
    std::lock_guard<std::mutex> lock(g_mock_storage_mutex);
    off_t new_pos = -1;
    if (whence == SEEK_SET) {
        new_pos = offset;
    } else if (whence == SEEK_CUR) {
        new_pos = g_mock_storage_pos + offset;
    } else if (whence == SEEK_END) {
        new_pos = static_cast<off_t>(g_mock_storage_data.size()) + offset;
    }

    if (new_pos >= 0) { 
        g_mock_storage_pos = new_pos;
    } else {
        return -1; // Invalid seek
    }
    return new_pos;
}

// Helper to reset mock storage before each test
void reset_mock_storage() {
    std::lock_guard<std::mutex> lock(g_mock_storage_mutex);
    g_mock_storage_data.clear();
    g_mock_storage_pos = 0;
}

// Test Case 1: Create and Destroy
void test_create_destroy() {
    reset_mock_storage();
    storage_operations_t mock_ops = {mock_write, mock_read, mock_lseek};
    storage_handle_t test_handle = 1;

    conveyor_t* conv = conveyor_create(test_handle, O_RDWR, mock_ops, 1024, 1024);
    assert(conv != nullptr);
    conveyor_destroy(conv);
}

// Test Case 2: Write and Flush
void test_write_and_flush() {
    reset_mock_storage();
    storage_operations_t mock_ops = {mock_write, mock_read, mock_lseek};
    storage_handle_t test_handle = 1;

    conveyor_t* conv = conveyor_create(test_handle, O_WRONLY, mock_ops, 1024, 0);
    assert(conv != nullptr);

    std::string test_data = "Hello, Conveyor!";
    ssize_t bytes_written = conveyor_write(conv, test_data.c_str(), test_data.length());
    assert(bytes_written == (ssize_t)test_data.length());

    conveyor_flush(conv);
    conveyor_destroy(conv);

    assert(g_mock_storage_data.size() == test_data.length());
    assert(std::string(g_mock_storage_data.data(), g_mock_storage_data.size()) == test_data);
}

// Test Case 3: Buffered Read
void test_buffered_read() {
    reset_mock_storage();
    storage_operations_t mock_ops = {mock_write, mock_read, mock_lseek};
    storage_handle_t test_handle = 1;

    std::string test_data = "This is a test of the buffered read functionality.";
    mock_write(test_handle, test_data.c_str(), test_data.length());
    mock_lseek(test_handle, 0, SEEK_SET);

    conveyor_t* conv = conveyor_create(test_handle, O_RDONLY, mock_ops, 0, 1024);
    assert(conv != nullptr);
    
    std::vector<char> read_buffer(test_data.length() + 1, '\0');
    ssize_t bytes_read = conveyor_read(conv, read_buffer.data(), test_data.length());
    
    assert(bytes_read == (ssize_t)test_data.length());
    assert(std::string(read_buffer.data()) == test_data);

    conveyor_destroy(conv);
}

// Test Case 4: Read after Write (O_RDWR)
void test_read_after_write() {
    reset_mock_storage();
    storage_operations_t mock_ops = {mock_write, mock_read, mock_lseek};
    storage_handle_t test_handle = 1;

    conveyor_t* conv = conveyor_create(test_handle, O_RDWR, mock_ops, 1024, 1024);
    assert(conv != nullptr);

    std::string write_data = "This is some data.";
    ssize_t bytes_written = conveyor_write(conv, write_data.c_str(), write_data.length());
    assert(bytes_written == (ssize_t)write_data.length());

    conveyor_flush(conv); 

    off_t seek_pos = conveyor_lseek(conv, 0, SEEK_SET);
    assert(seek_pos == 0);

    std::vector<char> read_buffer(write_data.length() + 1, '\0');
    ssize_t bytes_read = conveyor_read(conv, read_buffer.data(), write_data.length());
    
    assert(bytes_read == (ssize_t)write_data.length());
    assert(std::string(read_buffer.data()) == write_data);

    conveyor_destroy(conv);
}

// Test Case 5: Append Mode (O_APPEND)
void test_append_mode() {
    reset_mock_storage();
    storage_operations_t mock_ops = {mock_write, mock_read, mock_lseek};
    storage_handle_t test_handle = 1;

    std::string initial_data = "Initial data. ";
    mock_write(test_handle, initial_data.c_str(), initial_data.length());

    conveyor_t* conv = conveyor_create(test_handle, O_WRONLY | O_APPEND, mock_ops, 1024, 0);
    assert(conv != nullptr);

    std::string append_data = "Appended data.";
    ssize_t bytes_written = conveyor_write(conv, append_data.c_str(), append_data.length());
    assert(bytes_written == (ssize_t)append_data.length());

    conveyor_flush(conv);
    conveyor_destroy(conv);

    std::string expected_data = initial_data + append_data;
    assert(g_mock_storage_data.size() == expected_data.length());
    assert(std::string(g_mock_storage_data.data(), g_mock_storage_data.size()) == expected_data);
}


int main() {
    test_create_destroy();
    test_write_and_flush();
    test_buffered_read();
    test_read_after_write();
    test_append_mode();

    return 0;
}
