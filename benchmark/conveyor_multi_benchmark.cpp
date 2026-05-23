#include "libconveyor/conveyor.h"
#include <iostream>
#include <vector>
#include <chrono>
#include <thread>
#include <numeric>   
#include <algorithm> 
#include <cstring>
#include <atomic>
#include <fcntl.h>
#include <unistd.h>
#include <sys/stat.h>

// --- Configuration ---
const size_t BLOCK_SIZE = 64 * 1024;    // 64 KB blocks for large movement
const size_t TOTAL_DATA = 1024LL * 1024 * 1024; // 1 GB total
const int SIMULATED_LATENCY_US = 1000;   // 1ms backend latency
const int THREAD_COUNTS[] = {1, 2, 4, 8, 16};

// --- Slow Storage Wrapper ---
ssize_t slow_pwrite(storage_handle_t fd, const void* buf, size_t count, off_t offset) {
    if (SIMULATED_LATENCY_US > 0) {
        std::this_thread::sleep_for(std::chrono::microseconds(SIMULATED_LATENCY_US));
    }
    return ::pwrite((int)(intptr_t)fd, buf, count, offset);
}

ssize_t slow_pread(storage_handle_t fd, void* buf, size_t count, off_t offset) {
    if (SIMULATED_LATENCY_US > 0) {
        std::this_thread::sleep_for(std::chrono::microseconds(SIMULATED_LATENCY_US));
    }
    return ::pread((int)(intptr_t)fd, buf, count, offset);
}

off_t slow_lseek(storage_handle_t fd, off_t offset, int whence) {
    return lseek((int)(intptr_t)fd, offset, whence);
}

void run_parallel_benchmark(int num_threads) {
    std::string filename = "multi_bench_" + std::to_string(num_threads) + ".dat";
    int fd = open(filename.c_str(), O_RDWR | O_CREAT | O_TRUNC, 0666);
    if (fd < 0) { perror("open"); return; }
    ftruncate(fd, TOTAL_DATA);

    storage_operations_t ops = { slow_pwrite, slow_pread, slow_lseek };
    conveyor_config_t cfg = {0};
    cfg.handle = (storage_handle_t)(intptr_t)fd;
    cfg.flags = O_RDWR;
    cfg.ops = ops;
    cfg.initial_write_size = 256 * 1024 * 1024; // 256MB initial buffer
    cfg.max_write_size = 1024LL * 1024 * 1024; // 1GB max
    cfg.write_chunk_size = 32 * 1024 * 1024;   // 32MB chunks
    
    conveyor_t* conv = conveyor_create(&cfg);
    
    size_t data_per_thread = TOTAL_DATA / num_threads;
    std::vector<std::thread> threads;
    std::vector<char> block_data(BLOCK_SIZE, 'A');

    auto start = std::chrono::high_resolution_clock::now();

    for (int i = 0; i < num_threads; ++i) {
        threads.emplace_back([&]() {
            for (size_t written = 0; written < data_per_thread; written += BLOCK_SIZE) {
                conveyor_write(conv, block_data.data(), BLOCK_SIZE);
            }
        });
    }

    for (auto& t : threads) t.join();
    conveyor_flush(conv);

    auto end = std::chrono::high_resolution_clock::now();
    double duration_s = std::chrono::duration<double>(end - start).count();
    
    std::cout << "Threads: " << num_threads 
              << " | Time: " << duration_s << " s"
              << " | Throughput: " << (TOTAL_DATA / (1024.0 * 1024.0)) / duration_s << " MB/s\n";

    conveyor_destroy(conv);
    close(fd);
    unlink(filename.c_str());
}

int main() {
    std::cout << "--- libconveyor Parallel 1GB Handoff Benchmark ---\n";
    std::cout << "Total Data: 1024 MB\n";
    std::cout << "Backend Latency: " << SIMULATED_LATENCY_US << " us\n\n";

    for (int count : THREAD_COUNTS) {
        run_parallel_benchmark(count);
    }

    return 0;
}
