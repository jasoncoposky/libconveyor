#include "libconveyor/conveyor.h"
#include <iostream>
#include <vector>
#include <chrono>
#include <thread>
#include <numeric>   
#include <algorithm> 
#include <cstring>
#include <cmath>
#include <fcntl.h>

#ifdef _MSC_VER
#include <io.h>
#include <windows.h>

// --- FIX 1: Thread-Safe Atomic I/O for Windows ---
// Uses WriteFile/ReadFile with OVERLAPPED to avoid changing the global file pointer.
ssize_t pwrite(int fd, const void* buf, size_t count, off_t offset) {
    HANDLE hFile = (HANDLE)_get_osfhandle(fd);
    if (hFile == INVALID_HANDLE_VALUE) return -1;

    OVERLAPPED ov = { 0 };
    ov.Offset = offset & 0xFFFFFFFF;
    ov.OffsetHigh = (offset >> 32) & 0xFFFFFFFF;

    DWORD bytesWritten = 0;
    if (!WriteFile(hFile, buf, (DWORD)count, &bytesWritten, &ov)) {
        return -1;
    }
    return (ssize_t)bytesWritten;
}

ssize_t pread(int fd, void* buf, size_t count, off_t offset) {
    HANDLE hFile = (HANDLE)_get_osfhandle(fd);
    if (hFile == INVALID_HANDLE_VALUE) return -1;

    OVERLAPPED ov = { 0 };
    ov.Offset = offset & 0xFFFFFFFF;
    ov.OffsetHigh = (offset >> 32) & 0xFFFFFFFF;

    DWORD bytesRead = 0;
    if (!ReadFile(hFile, buf, (DWORD)count, &bytesRead, &ov)) {
        return -1;
    }
    return (ssize_t)bytesRead;
}

// Map MSVC functions
#define open _open
#define close _close
#define lseek _lseek
#define ftruncate _chsize
#define unlink _unlink

// Map Flags
#define O_RDWR _O_RDWR
#define O_CREAT _O_CREAT
#define O_TRUNC _O_TRUNC
// --- FIX 2: Binary Mode is Mandatory on Windows ---
#define O_BINARY _O_BINARY 

#else
// Linux/macOS standard includes
#include <unistd.h>
#include <sys/stat.h>
#define O_BINARY 0 // No-op on POSIX
#endif

// --- Configuration ---
const size_t BLOCK_SIZE = 4096;        
const size_t TOTAL_DATA = 10 * 1024 * 1024; // 10 MB total
const size_t NUM_OPS = TOTAL_DATA / BLOCK_SIZE;
const int SIMULATED_LATENCY_US = 2000; 

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
ssize_t slow_pwrite(storage_handle_t fd, const void* buf, size_t count, off_t offset) {
    if (SIMULATED_LATENCY_US > 0) {
        std::this_thread::sleep_for(std::chrono::microseconds(SIMULATED_LATENCY_US));
    }
#ifdef _MSC_VER
    return pwrite((int)(intptr_t)fd, buf, count, offset);
#else
    return ::pwrite((int)(intptr_t)fd, buf, count, offset);
#endif
}

ssize_t slow_pread(storage_handle_t fd, void* buf, size_t count, off_t offset) {
    if (SIMULATED_LATENCY_US > 0) {
        std::this_thread::sleep_for(std::chrono::microseconds(SIMULATED_LATENCY_US));
    }
#ifdef _MSC_VER
    return pread((int)(intptr_t)fd, buf, count, offset);
#else
    return ::pread((int)(intptr_t)fd, buf, count, offset);
#endif
}

off_t slow_lseek(storage_handle_t fd, off_t offset, int whence) {
    return lseek((int)(intptr_t)fd, offset, whence);
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
    
    // Create conveyor with 5MB buffers
    conveyor_config_t cfg = {0};
    cfg.handle = (storage_handle_t)(intptr_t)fd;
    cfg.flags = O_RDWR;
    cfg.ops = ops;
    cfg.initial_write_size = 20 * 1024 * 1024;
    cfg.initial_read_size = 1024 * 1024;
    cfg.max_write_size = cfg.initial_write_size;
    cfg.max_read_size = cfg.initial_read_size;
    conveyor_t* conv = conveyor_create(&cfg);
    
    std::vector<double> latencies;
    latencies.reserve(NUM_OPS);
    
    auto start_total = std::chrono::high_resolution_clock::now();
    
    for (size_t i = 0; i < NUM_OPS; ++i) {
        auto start_op = std::chrono::high_resolution_clock::now();
        
        ssize_t res = conveyor_write(conv, data.data(), BLOCK_SIZE);
        if (res < 0) {
            std::cerr << "Conveyor write failed! errno=" << errno << std::endl;
            exit(1);
        }

        auto end_op = std::chrono::high_resolution_clock::now();
        latencies.push_back(std::chrono::duration<double, std::micro>(end_op - start_op).count());
    }
    
    auto end_enqueue = std::chrono::high_resolution_clock::now();
    double enqueue_ms = std::chrono::duration<double, std::milli>(end_enqueue - start_total).count();
    
    auto flush_start = std::chrono::high_resolution_clock::now();
    conveyor_flush(conv); 
    auto flush_end = std::chrono::high_resolution_clock::now();
    double flush_ms = std::chrono::duration<double, std::milli>(flush_end - flush_start).count();
    std::cout << "  (Enqueue time: " << enqueue_ms << " ms, Flush time: " << flush_ms << " ms)\n";
    
    conveyor_destroy(conv);
    return calculate_stats(latencies, enqueue_ms);
}

int main() {
    std::cout << "Preparing Benchmark...\n";
    std::cout << "Block Size: " << BLOCK_SIZE << " bytes\n";
    std::cout << "Total Data: " << TOTAL_DATA / (1024*1024) << " MB\n";
    std::cout << "Simulated Backend Latency: " << SIMULATED_LATENCY_US / 1000.0 << " ms\n\n";

    std::vector<char> data(BLOCK_SIZE, 'X');
    
    // Add O_BINARY to prevent CRLF translation corruption
    int fd = open("benchmark_temp.dat", O_RDWR | O_CREAT | O_TRUNC | O_BINARY, 0666);
    if (fd < 0) { perror("open"); return 1; }

    std::cout << "Running Raw POSIX Benchmark (Blocking)...\n";
    Result raw_res = run_raw_write_benchmark(fd, data);
    print_result("Raw POSIX Write", raw_res);

    ftruncate(fd, 0);
    lseek(fd, 0, SEEK_SET);

    std::cout << "Running libconveyor Benchmark (Async)...\n";
    Result conv_res = run_conveyor_write_benchmark(fd, data);
    print_result("libconveyor Write", conv_res);

    close(fd);
    unlink("benchmark_temp.dat");
    
    double speedup = conv_res.throughput_mbs / raw_res.throughput_mbs;
    std::cout << ">>> SPEEDUP FACTOR: " << speedup << "x <<<";

    return 0;
}
