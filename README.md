# libconveyor: High-Performance I/O Buffering Library

[![License](https://img.shields.io/badge/License-BSD_3--Clause-blue.svg)](./LICENSE.md)

`libconveyor` is a high-performance, thread-safe C++ library designed to hide I/O latency through intelligent dual ring-buffering. It provides a POSIX-like API (`read`, `write`, `lseek`) while asynchronously managing data transfer to and from underlying storage via background worker threads.

## Features

*   **Dual Ring Buffers:** Separate, configurable buffers for write-behind caching and read-ahead prefetching. The write buffer now uses a **linear ring buffer** for optimal performance, eliminating per-write heap allocations.
*   **I/O Latency Hiding (Asynchronous Writes & Read-Ahead):** Asynchronous background threads perform actual storage operations, allowing application threads to proceed quickly. Writes are now zero-allocation on the hot path.
*   **Optimized Read-After-Write Consistency (Snooping):** The `conveyor_read` function efficiently "snoops" the write buffer, directly patching newly written data into read requests before it hits disk, ensuring immediate consistency without flushing.
*   **Robust Thread-Safety:**
    *   **Generation Counters:** Protect against `lseek` invalidating read buffers while `readWorker` is performing slow I/O, preventing data corruption.
    *   **Correct Lock Scoping:** Carefully managed mutex acquisition and release prevent deadlocks between application and worker threads.
    *   **Reduced Lock Contention:** The `writeWorker` is optimized to hold locks for minimal durations, copying data from the ring buffer and releasing the lock before performing slow I/O.
*   **`pread`/`pwrite` Semantics:** Interacts with underlying storage using stateless, offset-based `pread`/`pwrite` operations for robust multithreaded I/O.
*   **Pluggable Storage Backend:** Abstracted storage operations (`storage_operations_t`) allow `libconveyor` to be easily integrated with any block-storage mechanism (e.g., file systems, network storage APIs, custom drivers).
*   **Observability:** Provides detailed runtime statistics (`conveyor_stats_t`) including bytes transferred, latency, and buffer congestion events, with a "reset-on-read" model for windowed monitoring.
*   **Robust Error Handling:** Detects and reports the first asynchronous I/O error to the user via sticky error codes, with a mechanism to clear them (`conveyor_clear_error`).
*   **Fail-Fast for Invalid Writes:** Prevents indefinite hangs by failing writes that exceed the buffer's total capacity or timing out if space is not available.

## Modern C++17 Interface (`conveyor_modern.hpp`)

For modern C++ applications, `libconveyor` provides a header-only C++17 wrapper that offers enhanced safety, expressiveness, and a more portable API. It uses RAII, a custom `Result` type (mimicking `std::expected`) for error handling, and SFINAE for buffer safety, incurring zero runtime overhead.

### Key Improvements

#### 1. Safety via SFINAE & Container References
Instead of manually passing pointers and sizes, which risks buffer overflows, the modern API uses C++17's SFINAE (Substitution Failure Is Not An Error) and generic templates to accept `std::vector`, `std::string`, or `std::array` directly, making size mismatches impossible.

```cpp
// OLD C-style API
// conveyor_write(c, ptr, 100); // Risk: What if ptr only has 50 bytes?

// NEW C++17 API
std::vector<double> data = {1.1, 2.2, 3.3}; 
// Automatically calculates 3 * 8 = 24 bytes, impossible to mismatch
auto write_res = conveyor.write(data); 
```

#### 2. Error Handling via `Result<T>` (C++17 `std::variant`)
The new API returns a custom `Result<T>` object, which internally uses `std::variant`. This object either contains the result value or a strongly-typed `std::error_code`, eliminating the need to check for -1 and read the global `errno`.

```cpp
auto write_res = conveyor.write(data);
if (!write_res) {
    // write_res.error() contains the std::error_code
    std::cerr << "Write failed: " << write_res.error().message() << "\n";
}
```

#### 3. Automatic Resource Management (RAII)
The `Conveyor` class wraps the raw C-style pointer in a `std::unique_ptr` with a custom deleter. The destructor automatically calls `conveyor_destroy`, which in turn flushes any pending writes. This prevents resource leaks and data loss from forgetting to clean up.

### Modern Usage Example

```cpp
#include "libconveyor/conveyor_modern.hpp"
#include <iostream>
#include <vector>

// Dummy storage operations for the example
ssize_t my_pwrite(storage_handle_t, const void* buf, size_t count, off_t) {
    std::cout << "Storage: pwrite " << count << " bytes\n";
    return count;
}
ssize_t my_pread(storage_handle_t, void* buf, size_t count, off_t) {
    std::cout << "Storage: pread " << count << " bytes\n";
    std::memset(buf, 'D', count); // Fill with dummy data
    return count;
}
off_t my_lseek(storage_handle_t, off_t offset, int) {
    std::cout << "Storage: lseek to " << offset << "\n";
    return offset;
}

void cpp17_example_usage() {
    storage_operations_t ops = { my_pwrite, my_pread, my_lseek };
    
    // 1. Configure
    libconveyor::v2::Config cfg;
    cfg.handle = nullptr; // Using a dummy handle for this example
    cfg.ops = ops;
    cfg.write_capacity = 10 * 1024 * 1024; // 10MB

    // 2. Create (Factory)
    auto result = libconveyor::v2::Conveyor::create(cfg);
    if (!result) {
        std::cerr << "Init failed: " << result.error().message() << '\n';
        return;
    }
    
    // Move ownership
    auto conveyor = std::move(result.value());

    // 3. Write using std::vector (Safe!)
    std::vector<double> data = {1.1, 2.2, 3.3}; 
    // Automatically calculates 3 * 8 = 24 bytes
    auto write_res = conveyor.write(data); 
    
    if (!write_res) {
        std::cerr << "Write error: " << write_res.error().message() << '\n';
    } else {
        std::cout << "Wrote " << write_res.value() << " bytes\n";
    }

    // 4. Manual flush (optional, destructor does it too)
    conveyor.flush();

    // 5. Read data back (using std::vector<char>)
    std::vector<char> read_buffer(24); // Size to match data written
    auto read_res = conveyor.read(read_buffer);
    if (!read_res) {
        std::cerr << "Read error: " << read_res.error().message() << '\n';
    } else {
        std::cout << "Read " << read_res.value() << " bytes\n";
    }


} // ~Conveyor() runs -> conveyor_destroy() -> flush -> join threads
```

## Motivation

This library was developed as a foundational component to enhance I/O performance within data management systems, particularly for creating pass-through resource plugins for systems like iRODS. By buffering I/O, `libconveyor` can significantly reduce perceived latency for client applications, making operations feel more responsive even when interacting with slow or distant storage.

## High-Level Architecture

`libconveyor` operates by decoupling application I/O requests from the physical I/O operations.

1.  **Application Thread:** Calls `conveyor_read()`, `conveyor_write()`, or `conveyor_lseek()`.
    *   `conveyor_write()`: Copies data into a pre-allocated **write ring buffer** and pushes lightweight metadata (`WriteRequest`) to a queue, then immediately returns. This path is now zero-allocation.
    *   `conveyor_read()`: Prioritizes satisfying requests from the in-memory read buffer (filled by `readWorker`). It then "snoops" the write queue's metadata and patches data directly from the write ring buffer into the user's buffer if any overlaps with pending writes are found. If data is unavailable, it signals the `readWorker` and waits.
    *   `conveyor_lseek()`: Flushes pending writes, invalidates read buffers, updates internal file pointers, and increments a **generation counter** before performing the underlying seek.
2.  **`writeWorker` Thread (Write-Behind):** Runs in the background, consuming `WriteRequest` metadata from a queue. It retrieves the data from the write ring buffer (using `peek_at`), performs `ops.pwrite()` to the actual storage, and only then marks the space in the write ring buffer as free. This process is optimized for reduced lock contention.
3.  **`readWorker` Thread (Read-Ahead):** Runs in the background, proactively fetching data from storage using `ops.pread()` into its read ring buffer. It anticipates future reads to minimize latency, also checking the **generation counter** to discard stale data after a concurrent `lseek`.
4.  **`storage_operations_t`:** A set of function pointers (`pwrite`, `pread`, `lseek`) provided during `conveyor_create` that define how `libconveyor` interacts with the specific underlying storage backend.

## Building

`libconveyor` uses CMake for its build system. It includes comprehensive unit tests, stress tests (using Google Test), and performance benchmarks.

```bash
# Clone the repository and navigate into it
git clone <repository_url>
cd libconveyor

# Create a build directory and configure CMake
mkdir build
cd build
cmake ..

# Build the project (default is Debug configuration)
cmake --build .

# For Release build (recommended for performance testing)
cmake --build . --config Release
```

To run the tests:

```bash
# From the build directory
# Basic tests (custom harness)
./test/Debug/conveyor_basic_test.exe # On Windows
# ./test/Release/conveyor_basic_test.exe # On Windows
# ./test/conveyor_basic_test # On Linux/macOS

# Stress tests (Google Test)
./test/Debug/conveyor_stress_test.exe # On Windows
# ./test/Release/conveyor_stress_test.exe # On Windows
# ./test/conveyor_stress_test # On Linux/macOS
```

To run the performance benchmark:

```bash
# From the build directory (use Release build for meaningful results)
./benchmark/Release/conveyor_benchmark.exe # On Windows
# ./benchmark/conveyor_benchmark # On Linux/macOS
```

## Performance

`libconveyor` is designed to provide significant performance gains by hiding I/O latency, especially when an application can perform other work while writes are happening asynchronously in the background. A simulated benchmark was run with a 10MB total write, using 4KB blocks and a simulated backend latency of 2ms per block (actual sleep on Windows is ~15ms due to timer resolution). The conveyor was configured with a 20MB write buffer to absorb the entire burst.

### Benchmark Results

**Scenario:** Write 10MB of data in 4KB blocks.

| Benchmark                  | Total Time (ms) | Throughput (MB/s) | Avg Latency (us) | P99 Latency (us) |
| :------------------------- | :-------------- | :---------------- | :--------------- | :--------------- |
| **Raw POSIX Write** (Blocking) | 36739.7         | 0.272             | 14351.2          | 16588.1          |
| **libconveyor Write** (Async Enqueue) | 1.8625          | 5369.13           | 0.700            | 2.5              |

### Speedup Factor: 19726x

**Interpretation:**
The "Raw POSIX Write" scenario measures the time taken when the application thread is blocked, waiting for each 4KB write operation to complete (including the simulated 15ms latency). The total time is approximately `2560 writes * 14.3ms/write = ~36.7 seconds`.

The "libconveyor Write (Async Enqueue)" scenario measures only the time the application thread spends enqueuing the writes into `libconveyor`'s internal buffers. This process is extremely fast (milliseconds), as `libconveyor` immediately absorbs the data into its ring buffer and returns control to the application. The actual writing to the slow backend happens concurrently in `writeWorker` threads.

This benchmark vividly demonstrates `libconveyor`'s ability to nearly eliminate perceived I/O latency for the application thread during write bursts, allowing the application to achieve a massive speedup by performing other useful work instead of waiting for I/O. The `conveyor_flush` operation (which is not included in the "Async Enqueue" time but measured separately as 36506 ms) ensures all data eventually reaches persistent storage.

### Read Benchmark Results

**Scenario:** Read 10MB of data in 4KB blocks with a 2ms simulated backend latency. The conveyor was configured with a 5MB read buffer, designed to require only two "disk" accesses for the entire 10MB file.

| Benchmark                      | Total Time (ms) | Throughput (MB/s) | Avg Latency (us) |
| :----------------------------- | :-------------- | :---------------- | :--------------- |
| **Raw POSIX Read** (Blocking)  | 7095.91         | 1.40926           | 2768.12          |
| **libconveyor Read** (Prefetching) | 12.3602         | 809.048           | 4.72941          |

### Read Speedup Factor: 574.094x

**Interpretation:**
The "Raw POSIX Read" scenario involves the application thread directly calling a `pread` operation for each 4KB block. With a simulated 2ms latency per call, and 2560 blocks (10MB / 4KB), the total time is dominated by `2560 * 2ms = 5120ms`. The higher actual total time of ~7 seconds indicates additional overhead beyond just the simulated latency.

The "libconveyor Read (Prefetching)" scenario demonstrates the effectiveness of the read-ahead buffer. The application thread performs `conveyor_read`, which is primarily satisfied by the in-memory read buffer. The `readWorker` thread, running in the background, proactively fetches large chunks (5MB at a time in this setup) from the slow storage. This significantly reduces the number of times the application thread has to wait for slow I/O, resulting in a dramatic reduction in perceived latency and a massive increase in throughput. The application thread experiences very low latency because most reads are served from the fast in-memory buffer.

This benchmark highlights `libconveyor`'s capability to mitigate the impact of high-latency storage on read operations by prefetching data and serving application requests from a fast in-memory cache.

## Usage Example (Conceptual)

```cpp
#include "libconveyor/conveyor.h"
#include <iostream>
#include <string>
#include <vector>
#include <fcntl.h> // For O_RDWR, etc.
#include <unistd.h> // For close, unlink (POSIX)
#include <sys/stat.h> // For file permissions (POSIX)

// --- Windows-specific includes and mappings for example ---
#ifdef _MSC_VER
#include <io.h>
#define open _open
#define close _close
#define unlink _unlink
#define S_IREAD _S_IREAD
#define S_IWRITE _S_IWRITE
#define O_BINARY _O_BINARY // Binary mode is crucial for Windows
#endif
// --- End Windows-specific ---

// Example mock storage operations (these would typically interact with a real file system or network)
ssize_t my_pwrite(storage_handle_t fd, const void* buf, size_t count, off_t offset) {
    // In a real scenario, this would write to the actual storage medium.
    // For this example, we just simulate the work.
    std::cout << "Storage: pwrite " << count << " bytes at " << offset << std::endl;
    // Simulate some work or latency
    // std::this_thread::sleep_for(std::chrono::milliseconds(10));
    return count;
}

ssize_t my_pread(storage_handle_t fd, void* buf, size_t count, off_t offset) {
    // In a real scenario, this would read from the actual storage medium.
    // For this example, we just fill with dummy data.
    std::cout << "Storage: pread " << count << " bytes at " << offset << std::endl;
    std::memset(buf, 'R', count); // Fill with 'R' for Read
    // Simulate some work or latency
    // std::this_thread::sleep_for(std::chrono::milliseconds(10));
    return count;
}

off_t my_lseek(storage_handle_t fd, off_t offset, int whence) {
    // In a real scenario, this would perform the actual seek on the storage.
    std::cout << "Storage: lseek to " << offset << " with whence " << whence << std::endl;
    // This example just returns the offset for SEEK_SET; real lseek is more complex.
    if (whence == SEEK_SET) return offset;
    // For a mock, a proper implementation would need to track file size.
    return 0; 
}

int main() {
    storage_operations_t my_storage_ops = {my_pwrite, my_pread, my_lseek};
    
    // Create a temporary file for the mock storage backend to interact with
    // Use O_BINARY on Windows to prevent text mode issues
    int temp_fd = open("example_storage.bin", O_RDWR | O_CREAT | O_TRUNC | O_BINARY, S_IREAD | S_IWRITE);
    if (temp_fd < 0) {
        perror("Failed to open example_storage.bin");
        return 1;
    }

    // Create a conveyor with 1MB write buffer and 512KB read buffer
    conveyor_t* conv = conveyor_create(
        (storage_handle_t)(intptr_t)temp_fd, // Pass the actual file descriptor as handle
        O_RDWR, // Open for read/write
        &my_storage_ops,
        1024 * 1024, // 1MB write buffer
        512 * 1024   // 512KB read buffer
    );

    if (!conv) {
        std::cerr << "Failed to create conveyor."
;        close(temp_fd);
        unlink("example_storage.bin");
        return 1;
    }

    std::string data_to_write = "Hello, buffered world!";
    std::cout << "Application: Writing '" << data_to_write << "'...";
    conveyor_write(conv, data_to_write.c_str(), data_to_write.length());
    
    // Data is now in the buffer, writeWorker will flush it asynchronously.
    // Application thread continues immediately.
    std::cout << "Application: Write enqueued, application continuing work."
;    std::this_thread::sleep_for(std::chrono::milliseconds(50)); // Simulate other work

    // Force flush to ensure data is written before destroy
    std::cout << "Application: Flushing conveyor..."
;    conveyor_flush(conv);
    std::cout << "Application: Conveyor flushed."
;    
    // Now, try to read the data back
    char read_buffer[100];
    off_t seek_res = conveyor_lseek(conv, 0, SEEK_SET);
    if (seek_res == LIBCONVEYOR_ERROR) {
        std::cerr << "Failed to lseek."
;        conveyor_destroy(conv); close(temp_fd); unlink("example_storage.bin"); return 1;
    }
    
    ssize_t bytes_read = conveyor_read(conv, read_buffer, data_to_write.length());
    if (bytes_read == LIBCONVEYOR_ERROR) {
        std::cerr << "Failed to read."
;        conveyor_destroy(conv); close(temp_fd); unlink("example_storage.bin"); return 1;
    }
    read_buffer[bytes_read] = '\0';
    std::cout << "Application: Read '" << read_buffer << "'"
;    
    // Get and print some stats
    conveyor_stats_t stats;
    if (conveyor_get_stats(conv, &stats) == 0) {
        std::cout << "Bytes written in window: " << stats.bytes_written << std::endl;
        std::cout << "Avg write latency: " << stats.avg_write_latency_ms << "us"
;
    }

    conveyor_destroy(conv);
    close(temp_fd);
    unlink("example_storage.bin");
    return 0;
}
```

## Contributing


Contributions are welcome! Please ensure that any new features or bug fixes include corresponding test cases and maintain the existing coding style.

## License

`libconveyor` is distributed under the **BSD 3-Clause License**. See the [LICENSE.md](./LICENSE.md) file for full details.