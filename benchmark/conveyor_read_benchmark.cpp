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

// --- Windows Compatibility Wrappers (Same as write benchmark) ---
ssize_t pwrite_safe(int fd, const void* buf, size_t count, off_t offset) {
    HANDLE hFile = (HANDLE)_get_osfhandle(fd);
    if (hFile == INVALID_HANDLE_VALUE) return -1;
    OVERLAPPED ov = { 0 };
    ov.Offset = offset & 0xFFFFFFFF;
    ov.OffsetHigh = (offset >> 32) & 0xFFFFFFFF;
    DWORD bytesWritten = 0;
    if (!WriteFile(hFile, buf, (DWORD)count, &bytesWritten, &ov)) return -1;
    return (ssize_t)bytesWritten;
}

ssize_t pread_safe(int fd, void* buf, size_t count, off_t offset) {
    HANDLE hFile = (HANDLE)_get_osfhandle(fd);
    if (hFile == INVALID_HANDLE_VALUE) return -1;
    OVERLAPPED ov = { 0 };
    ov.Offset = offset & 0xFFFFFFFF;
    ov.OffsetHigh = (offset >> 32) & 0xFFFFFFFF;
    DWORD bytesRead = 0;
    if (!ReadFile(hFile, buf, (DWORD)count, &bytesRead, &ov)) return -1;
    return (ssize_t)bytesRead;
}

#define open _open
#define close _close
#define lseek _lseek
#define ftruncate _chsize
#define unlink _unlink
#define O_RDWR _O_RDWR
#define O_CREAT _O_CREAT
#define O_TRUNC _O_TRUNC
#define O_BINARY _O_BINARY 
#else
#include <unistd.h>
#include <sys/stat.h>
#define O_BINARY 0 
#define pwrite_safe ::pwrite
#define pread_safe ::pread
#endif

// --- Configuration ---
const size_t BLOCK_SIZE = 4096;        // Application reads in 4KB chunks
const size_t TOTAL_DATA = 10 * 1024 * 1024; // 10 MB file
const size_t NUM_OPS = TOTAL_DATA / BLOCK_SIZE;
const int SIMULATED_LATENCY_US = 2000; // 2ms simulated latency per syscall

// --- Statistics Helper ---
struct Result {
    double total_time_ms;
    double throughput_mbs;
    double avg_latency_us;
};

Result calculate_stats(const std::vector<double>& latencies_us, double total_time_ms) {
    Result r;
    r.total_time_ms = total_time_ms;
    r.throughput_mbs = (double)TOTAL_DATA / (1024.0 * 1024.0) / (total_time_ms / 1000.0);
    double sum = std::accumulate(latencies_us.begin(), latencies_us.end(), 0.0);
    r.avg_latency_us = sum / latencies_us.size();
    return r;
}

void print_result(const std::string& name, const Result& r) {
    std::cout << "--------------------------------------------------\n";
    std::cout << "BENCHMARK: " << name << "\n";
    std::cout << "--------------------------------------------------\n";
    std::cout << "  Total Time:    " << r.total_time_ms << " ms\n";
    std::cout << "  Throughput:    " << r.throughput_mbs << " MB/s\n";
    std::cout << "  Avg Latency:   " << r.avg_latency_us << " us\n";
    std::cout << "--------------------------------------------------\n\n";
}

// --- Slow Storage Wrapper ---
// NOTE: Latency applies per CALL. 
// Raw reads call this 2560 times (2560 * 2ms).
// Conveyor worker calls this ~2 times (2 * 2ms) because it fetches 5MB at a time.
ssize_t slow_pwrite(storage_handle_t fd, const void* buf, size_t count, off_t offset) {
    // No latency on write for this test, we are testing READ speed.
    // We want to fill the file fast to start the test.
    return pwrite_safe((int)(intptr_t)fd, buf, count, offset);
}

ssize_t slow_pread(storage_handle_t fd, void* buf, size_t count, off_t offset) {
    if (SIMULATED_LATENCY_US > 0) {
        std::this_thread::sleep_for(std::chrono::microseconds(SIMULATED_LATENCY_US));
    }
    return pread_safe((int)(intptr_t)fd, buf, count, offset);
}

off_t slow_lseek(storage_handle_t fd, off_t offset, int whence) {
    return lseek((int)(intptr_t)fd, offset, whence);
}

// --- Benchmarks ---

Result run_raw_read_benchmark(int fd) {
    std::vector<double> latencies;
    latencies.reserve(NUM_OPS);
    std::vector<char> buf(BLOCK_SIZE);
    
    auto start_total = std::chrono::high_resolution_clock::now();
    
    for (size_t i = 0; i < NUM_OPS; ++i) {
        auto start_op = std::chrono::high_resolution_clock::now();
        
        slow_pread((storage_handle_t)(intptr_t)fd, buf.data(), BLOCK_SIZE, i * BLOCK_SIZE);
        
        auto end_op = std::chrono::high_resolution_clock::now();
        latencies.push_back(std::chrono::duration<double, std::micro>(end_op - start_op).count());
    }
    
    auto end_total = std::chrono::high_resolution_clock::now();
    return calculate_stats(latencies, std::chrono::duration<double, std::milli>(end_total - start_total).count());
}

Result run_conveyor_read_benchmark(int fd) {
    storage_operations_t ops = { slow_pwrite, slow_pread, slow_lseek };
    
    // Create conveyor with 5MB Read Buffer
    // 5MB buffer means it should only need to hit the "disk" twice to read 10MB.
    conveyor_t* conv = conveyor_create((storage_handle_t)(intptr_t)fd, O_RDONLY, &ops, 0, 5 * 1024 * 1024);
    
    std::vector<double> latencies;
    latencies.reserve(NUM_OPS);
    std::vector<char> buf(BLOCK_SIZE);
    
    auto start_total = std::chrono::high_resolution_clock::now();
    
    for (size_t i = 0; i < NUM_OPS; ++i) {
        auto start_op = std::chrono::high_resolution_clock::now();
        
        ssize_t res = conveyor_read(conv, buf.data(), BLOCK_SIZE);
        if (res <= 0) {
            std::cerr << "Read failed or EOF at block " << i << std::endl;
            break;
        }

        auto end_op = std::chrono::high_resolution_clock::now();
        latencies.push_back(std::chrono::duration<double, std::micro>(end_op - start_op).count());
    }
    
    auto end_total = std::chrono::high_resolution_clock::now();
    conveyor_destroy(conv);
    return calculate_stats(latencies, std::chrono::duration<double, std::milli>(end_total - start_total).count());
}

int main() {
    std::cout << "Preparing Read Benchmark...\n";
    std::cout << "File Size: " << TOTAL_DATA / (1024*1024) << " MB\n";
    std::cout << "Read Block Size: " << BLOCK_SIZE << " bytes (Simulating chatty reads)\n";
    std::cout << "Simulated Backend Latency: " << SIMULATED_LATENCY_US / 1000.0 << " ms\n";

    // 1. Create and populate file
    int fd = open("benchmark_read.dat", O_RDWR | O_CREAT | O_TRUNC | O_BINARY, 0666);
    if (fd < 0) { perror("open"); return 1; }
    
    std::cout << "Populating file (no latency)...\n";
    std::vector<char> big_buffer(1024 * 1024, 'A'); // 1MB chunks
    for (int i = 0; i < 10; i++) {
        pwrite_safe(fd, big_buffer.data(), big_buffer.size(), i * 1024 * 1024);
    }

    // 2. Run RAW
    std::cout << "\nRunning Raw POSIX Read (Blocking)...\n";
    Result raw_res = run_raw_read_benchmark(fd);
    print_result("Raw POSIX Read", raw_res);

    // 3. Run CONVEYOR
    std::cout << "Running libconveyor Read (Prefetching)...\n";
    Result conv_res = run_conveyor_read_benchmark(fd);
    print_result("libconveyor Read", conv_res);

    // Cleanup
    close(fd);
    unlink("benchmark_read.dat");
    
    double speedup = conv_res.throughput_mbs / raw_res.throughput_mbs;
    std::cout << ">>> READ SPEEDUP FACTOR: " << speedup << "x <<<\n";
    
    return 0;
}