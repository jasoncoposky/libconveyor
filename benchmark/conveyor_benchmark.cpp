#include "libconveyor/conveyor.h"
#include <iostream>
#include <vector>
#include <chrono>
#include <thread>
#include <numeric>   // For std::accumulate
#include <algorithm> // For std::sort
#include <cstring>
#include <cmath>
#include <fcntl.h>

#ifdef _MSC_VER
#include <io.h>
#include <windows.h>

// POSIX pwrite/pread are not available on Windows, so we emulate them
ssize_t pwrite(int fd, const void* buf, size_t count, off_t offset) {
    long original_pos = _lseek(fd, 0, SEEK_CUR);
    if (original_pos == -1) return -1;
    if (_lseek(fd, offset, SEEK_SET) == -1) return -1;
    ssize_t bytes_written = _write(fd, buf, count);
    _lseek(fd, original_pos, SEEK_SET);
    return bytes_written;
}

ssize_t pread(int fd, void* buf, size_t count, off_t offset) {
    long original_pos = _lseek(fd, 0, SEEK_CUR);
    if (original_pos == -1) return -1;
    if (_lseek(fd, offset, SEEK_SET) == -1) return -1;
    ssize_t bytes_read = _read(fd, buf, count);
    _lseek(fd, original_pos, SEEK_SET);
    return bytes_read;
}

// Map other functions and macros
#define open _open
#define close _close
#define lseek _lseek
#define ftruncate _chsize
#define unlink _unlink
#define O_RDWR _O_RDWR
#define O_CREAT _O_CREAT
#define O_TRUNC _O_TRUNC
#define S_IREAD _S_IREAD
#define S_IWRITE _S_IWRITE
#endif



// --- Configuration ---
const size_t BLOCK_SIZE = 4096;        // 4KB blocks
const size_t TOTAL_DATA = 10 * 1024 * 1024; // 10 MB total
const size_t NUM_OPS = TOTAL_DATA / BLOCK_SIZE;
const int SIMULATED_LATENCY_US = 2000; // 2ms latency per IO (Network drive simulation)

// --- Helper: Statistics ---
struct Result {
    double total_time_ms;
    double throughput_mbs;
    double avg_latency_us;
    double p99_latency_us;
};

Result calculate_stats(const std::vector<double>& latencies_us, double total_time_ms) {
    Result r;
    r.total_time_ms = total_time_ms;
    r.throughput_mbs = (double)TOTAL_DATA / (1024.0 * 1024.0) / (total_time_ms / 1000.0);
    
    double sum = std::accumulate(latencies_us.begin(), latencies_us.end(), 0.0);
    r.avg_latency_us = sum / latencies_us.size();

    std::vector<double> sorted = latencies_us;
    std::sort(sorted.begin(), sorted.end());
    r.p99_latency_us = sorted[(size_t)(sorted.size() * 0.99)];
    
    return r;
}

void print_result(const std::string& name, const Result& r) {
    std::cout << "--------------------------------------------------\n";
    std::cout << "BENCHMARK: " << name << "\n";
    std::cout << "--------------------------------------------------\n";
    std::cout << "  Total Time:    " << r.total_time_ms << " ms\n";
    std::cout << "  Throughput:    " << r.throughput_mbs << " MB/s\n";
    std::cout << "  Avg Latency:   " << r.avg_latency_us << " us\n";
    std::cout << "  P99 Latency:   " << r.p99_latency_us << " us\n";
    std::cout << "--------------------------------------------------\n\n";
}

// --- Slow Storage Wrapper ---
// Wraps real POSIX calls but adds artificial delay to simulate network/disk load
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
    return ::lseek((int)(intptr_t)fd, offset, whence);
}

// --- Benchmarks ---

Result run_raw_write_benchmark(int fd, const std::vector<char>& data) {
    std::vector<double> latencies;
    latencies.reserve(NUM_OPS);
    
    auto start_total = std::chrono::high_resolution_clock::now();
    
    for (size_t i = 0; i < NUM_OPS; ++i) {
        auto start_op = std::chrono::high_resolution_clock::now();
        
        slow_pwrite((storage_handle_t)(intptr_t)fd, data.data(), BLOCK_SIZE, i * BLOCK_SIZE);
        
        auto end_op = std::chrono::high_resolution_clock::now();
        latencies.push_back(std::chrono::duration<double, std::micro>(end_op - start_op).count());
    }
    
    auto end_total = std::chrono::high_resolution_clock::now();
    double total_ms = std::chrono::duration<double, std::milli>(end_total - start_total).count();
    
    return calculate_stats(latencies, total_ms);
}

Result run_conveyor_write_benchmark(int fd, const std::vector<char>& data) {
    storage_operations_t ops = { slow_pwrite, slow_pread, slow_lseek };
    
    // Create conveyor with 5MB buffers (enough to hold half the test in RAM)
    conveyor_t* conv = conveyor_create((storage_handle_t)(intptr_t)fd, O_RDWR, &ops, 5 * 1024 * 1024, 1024 * 1024);
    
    std::vector<double> latencies;
    latencies.reserve(NUM_OPS);
    
    auto start_total = std::chrono::high_resolution_clock::now();
    
    for (size_t i = 0; i < NUM_OPS; ++i) {
        auto start_op = std::chrono::high_resolution_clock::now();
        
        // This should return INSTANTLY because it hits the buffer
        ssize_t res = conveyor_write(conv, data.data(), BLOCK_SIZE);
        if (res < 0) {
            std::cerr << "Conveyor write failed! errno=" << errno << std::endl;
            exit(1);
        }

        // Simulate other work happening in the application
        std::this_thread::sleep_for(std::chrono::milliseconds(1));

        auto end_op = std::chrono::high_resolution_clock::now();
        latencies.push_back(std::chrono::duration<double, std::micro>(end_op - start_op).count());
    }
    
    // We do NOT include flush time in latency stats because the application
    // perceives the write as "done". However, we must flush before destroying
    // to ensure correctness.
    conveyor_flush(conv); 

    auto end_total = std::chrono::high_resolution_clock::now();
    double total_ms = std::chrono::duration<double, std::milli>(end_total - start_total).count();
    
    conveyor_destroy(conv);
    return calculate_stats(latencies, total_ms);
}

int main() {
    std::cout << "Preparing Benchmark...\n";
    std::cout << "Block Size: " << BLOCK_SIZE << " bytes\n";
    std::cout << "Total Data: " << TOTAL_DATA / (1024*1024) << " MB\n";
    std::cout << "Simulated Backend Latency: " << SIMULATED_LATENCY_US / 1000.0 << " ms\n\n";

    std::vector<char> data(BLOCK_SIZE, 'X');
    
    // Create a temp file
    int fd = open("benchmark_temp.dat", O_RDWR | O_CREAT | O_TRUNC, 0666);
    if (fd < 0) { perror("open"); return 1; }

    // --- Run RAW ---
    std::cout << "Running Raw POSIX Benchmark (Blocking)...\n";
    Result raw_res = run_raw_write_benchmark(fd, data);
    print_result("Raw POSIX Write", raw_res);

    // Reset file
    ftruncate(fd, 0);
    lseek(fd, 0, SEEK_SET);

    // --- Run CONVEYOR ---
    std::cout << "Running libconveyor Benchmark (Async)...\n";
    Result conv_res = run_conveyor_write_benchmark(fd, data);
    print_result("libconveyor Write", conv_res);

    // --- Cleanup ---
    close(fd);
    unlink("benchmark_temp.dat");
    
    // --- Summary ---
    double speedup = conv_res.throughput_mbs / raw_res.throughput_mbs;
    std::cout << ">>> SPEEDUP FACTOR: " << speedup << "x <<<\n";
    std::cout << "(Note: Higher speedup means the application was blocked less often)\n";

    return 0;
}