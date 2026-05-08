#include "libconveyor/conveyor.h"
#include <iostream>
#include <vector>
#include <chrono>
#include <thread>
#include <atomic>
#include <numeric>
#include <algorithm>
#include <cstring>
#include <fcntl.h>
#include <unistd.h>
#include <sys/stat.h>

// --- Configuration ---
const size_t BLOCK_SIZE = 4096;
const size_t TOTAL_DATA = 20 * 1024 * 1024; // 20 MB total
const int SIMULATED_LATENCY_US = 1000;      // 1ms backend latency

// Mock Storage with Latency
struct MockStorage {
    std::vector<char> data;
    std::atomic<off_t> size{0};

    static ssize_t pwrite_callback(storage_handle_t h, const void* buf, size_t count, off_t offset) {
        auto* self = static_cast<MockStorage*>(h);
        if (SIMULATED_LATENCY_US > 0) std::this_thread::sleep_for(std::chrono::microseconds(SIMULATED_LATENCY_US));
        
        if (offset + count > self->data.size()) self->data.resize(offset + count);
        std::memcpy(self->data.data() + offset, buf, count);
        if (offset + count > self->size.load()) self->size = offset + count;
        return count;
    }

    static ssize_t pread_callback(storage_handle_t h, void* buf, size_t count, off_t offset) {
        auto* self = static_cast<MockStorage*>(h);
        if (SIMULATED_LATENCY_US > 0) std::this_thread::sleep_for(std::chrono::microseconds(SIMULATED_LATENCY_US));

        if (offset >= (off_t)self->data.size()) return 0;
        size_t available = std::min(count, (size_t)(self->data.size() - offset));
        std::memcpy(buf, self->data.data() + offset, available);
        return available;
    }

    static off_t lseek_callback(storage_handle_t h, off_t offset, int whence) {
        auto* self = static_cast<MockStorage*>(h);
        if (whence == SEEK_END) return self->size.load() + offset;
        return offset; // Simplified
    }
};

int main() {
    std::cout << "--- libconveyor Mixed Workload Benchmark ---\n";
    std::cout << "Simulating: Background Writer + Concurrent Reader\n";
    std::cout << "Backend Latency: " << SIMULATED_LATENCY_US << " us\n\n";

    MockStorage storage;
    storage.data.reserve(TOTAL_DATA);
    storage_operations_t ops = {MockStorage::pwrite_callback, MockStorage::pread_callback, MockStorage::lseek_callback};

    conveyor_config_t cfg = {0};
    cfg.handle = &storage;
    cfg.ops = ops;
    cfg.flags = O_RDWR;
    cfg.initial_write_size = 1024 * 1024; // 1MB write buffer
    cfg.initial_read_size = 1024 * 1024;  // 1MB read buffer
    cfg.max_write_size = 4 * 1024 * 1024;
    cfg.max_read_size = 4 * 1024 * 1024;

    conveyor_t* conv = conveyor_create(&cfg);

    std::atomic<bool> stop_writer{false};
    std::atomic<size_t> writes_completed{0};
    std::atomic<size_t> reads_completed{0};

    // 1. Background Writer Thread
    std::thread writer([&]() {
        char buf[BLOCK_SIZE];
        std::memset(buf, 'W', BLOCK_SIZE);
        while (!stop_writer) {
            if (conveyor_write(conv, buf, BLOCK_SIZE) > 0) {
                writes_completed += BLOCK_SIZE;
            } else {
                std::this_thread::yield();
            }
            if (writes_completed >= TOTAL_DATA) break;
        }
    });

    // 2. Concurrent Reader Thread
    // Small delay to let writer start
    std::this_thread::sleep_for(std::chrono::milliseconds(100));

    auto start_time = std::chrono::high_resolution_clock::now();
    
    std::thread reader([&]() {
        char buf[BLOCK_SIZE];
        off_t read_offset = 0;
        while (reads_completed < TOTAL_DATA) {
            // Try to read what has been written
            if (read_offset < (off_t)writes_completed.load()) {
                ssize_t n = conveyor_read(conv, buf, BLOCK_SIZE);
                if (n > 0) {
                    reads_completed += n;
                    read_offset += n;
                }
            } else {
                if (writes_completed >= TOTAL_DATA) break;
                std::this_thread::yield();
            }
        }
    });

    writer.join();
    reader.join();

    auto end_time = std::chrono::high_resolution_clock::now();
    double duration_ms = std::chrono::duration<double, std::milli>(end_time - start_time).count();

    std::cout << "Results:\n";
    std::cout << "  Total Data Processed: " << (TOTAL_DATA / (1024*1024)) << " MB write + " << (TOTAL_DATA / (1024*1024)) << " MB read\n";
    std::cout << "  Duration:             " << duration_ms << " ms\n";
    std::cout << "  Combined Throughput:  " << (2.0 * TOTAL_DATA / (1024.0*1024.0)) / (duration_ms / 1000.0) << " MB/s\n";

    conveyor_stats_t stats;
    conveyor_get_stats(conv, &stats);
    std::cout << "  Avg Write Latency:    " << stats.avg_write_latency_ms << " ms (Async)\n";
    std::cout << "  Avg Read Latency:     " << stats.avg_read_latency_ms << " ms (Prefetched)\n";

    conveyor_destroy(conv);
    return 0;
}
