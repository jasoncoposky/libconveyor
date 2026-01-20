<p align="center">
  <img src="assets/libconveyor_logo.jpg" alt="libconveyor Logo" width="500"/>
</p>

# libconveyor: High-Performance I/O Buffering Library

[![License](https://img.shields.io/badge/License-BSD_3--Clause-blue.svg)](./LICENSE.md)

`libconveyor` is a high-performance, thread-safe C++ library designed to hide I/O latency through intelligent dual ring-buffering. It provides a POSIX-like API (`read`, `write`, `lseek`) while asynchronously managing data transfer to and from underlying storage via background worker threads.

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

The **write ring buffer** and **read ring buffer** now dynamically adjust their sizes based on observed I/O patterns and demand, up to a configurable maximum.

## Features

*   **Dual Ring Buffers:** Separate, configurable buffers for write-behind caching and read-ahead prefetching. The write buffer now uses a **linear ring buffer** for optimal performance, eliminating per-write heap allocations.
*   **Adaptive Buffer Sizing:** Dynamically adjusts the size of the internal write and read buffers based on observed I/O patterns and demand. This feature automatically grows buffers when needed (e.g., large writes, sequential reads exhausting the read buffer) to optimize throughput, up to a user-defined `max_write_capacity` and `max_read_capacity`.
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

## Usage

`libconveyor` provides two interfaces: a modern C++17 wrapper (recommended) and a classic C-style API.

### Modern C++17 Interface (`conveyor_modern.hpp`)

The header-only C++17 wrapper is the recommended way to use `libconveyor`. It offers enhanced safety, expressiveness, and a more portable API by using RAII, a custom `Result` type (mimicking `std::expected`) for error handling, and SFINAE for buffer safety, all with zero runtime overhead.

#### Key Improvements

*   **RAII for Resource Management:** The `Conveyor` class wraps the raw C-style pointer in a `std::unique_ptr` with a custom deleter. The destructor automatically calls `conveyor_destroy`, which flushes pending writes and joins worker threads, preventing resource leaks and data loss.
*   **`Result<T>` for Error Handling:** The API returns a custom `Result<T>` object, which internally uses `std::variant`. This object either contains a valid result or a strongly-typed `std::error_code`, eliminating the need to check for -1 and read the global `errno`.
*   **Type-Safe Buffers:** Instead of manually passing pointers and sizes, the modern API uses C++17's SFINAE to accept standard library contiguous containers like `std::vector`, `std::string`, or `std::array` directly, making buffer overflow errors from size mismatches impossible.

#### C++17 Example

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
    auto write_res = conveyor.write(data); 
    
    if (!write_res) {
        std::cerr << "Write error: " << write_res.error().message() << '\n';
    } else {
        std::cout << "Wrote " << write_res.value() << " bytes\n";
    }

    // 4. Manual flush (optional, destructor does it too)
    conveyor.flush();

    // 5. Read data back
    std::vector<char> read_buffer(24); // Size to match data written
    auto read_res = conveyor.read(read_buffer);
    if (!read_res) {
        std::cerr << "Read error: " << read_res.error().message() << '\n';
    } else {
        std::cout << "Read " << read_res.value() << " bytes\n";
    }
} // ~Conveyor() runs -> conveyor_destroy() -> flush -> join threads
```

### C-Style API (`conveyor.h`)

The C API provides a stable, POSIX-like interface for maximum compatibility.

#### C API Example

```cpp
#include "libconveyor/conveyor.h"
#include <stdio.h>
#include <string.h>

// Mock functions (my_pwrite, my_pread, my_lseek) would be defined here...

void c_example_usage() {
    storage_operations_t my_storage_ops = { /* .pwrite = */ my_pwrite, /* ... */ };
    
    // Configure the conveyor
    conveyor_config_t cfg = {0};
    cfg.handle = nullptr; // dummy handle
    cfg.flags = O_RDWR;
    cfg.ops = my_storage_ops;
    cfg.initial_write_size = 1024 * 1024; // 1MB initial write buffer
    cfg.max_write_size = 2 * 1024 * 1024; // Max 2MB write buffer
    cfg.initial_read_size = 512 * 1024;   // 512KB initial read buffer
    cfg.max_read_size = 1 * 1024 * 1024;  // Max 1MB read buffer

    conveyor_t* conv = conveyor_create(&cfg);

    if (!conv) {
        perror("Failed to create conveyor");
        return;
    }

    const char* data = "Hello, C API!";
    conveyor_write(conv, data, strlen(data));
    conveyor_flush(conv);
    
    printf("Data written and flushed.\n");

    conveyor_destroy(conv);
}
```

## Observability and Monitoring

`libconveyor` provides a rich set of metrics through a "reset-on-read" statistics-gathering mechanism, enabling real-time monitoring, performance tuning, and easier debugging.

### Accessing Statistics

Statistics are accessed via the `conveyor_get_stats()` function (in the C API) or the `stats()` method (in the C++17 wrapper). When called, the function returns a snapshot of the metrics and atomically resets the internal counters to zero. This model is ideal for windowed monitoring, allowing an application to periodically poll the stats to get a clear picture of performance over the last interval.

The `conveyor_stats_t` struct (or `libconveyor::v2::Stats`) provides the following key metrics:

*   **`bytes_written` & `bytes_read`**: The total number of bytes written to and read from the conveyor during the last interval. Useful for calculating throughput.
*   **`avg_write_latency_ms` & `avg_read_latency_ms`**: The average latency of the underlying `pwrite` and `pread` operations, as observed by the worker threads. This helps isolate backend storage performance from the application's perceived latency.
*   **`write_buffer_full_events`**: A counter that increments each time a `conveyor_write` call has to wait because the write buffer is full. A consistently high value indicates that the application is producing data faster than the backend storage can consume it.
*   **`last_error_code`**: Captures the `errno` of the first I/O error that occurs in a background worker thread. This "sticky" error code is crucial for diagnosing otherwise silent backend failures.

### Potential Use Cases

The observability features of `libconveyor` unlock several powerful use cases:

1.  **Real-time Performance Monitoring:**
    *   **Dashboarding:** The stats can be exported to a time-series database (e.g., Prometheus, InfluxDB) and visualized in a dashboard (e.g., Grafana) to provide a real-time view of I/O throughput, latency, and buffer health.
    *   **Alerting:** Automated alerts can be configured to trigger on key metrics. For example, an alert could be fired if `avg_write_latency_ms` exceeds a certain threshold, if `write_buffer_full_events` is persistently high, or if `last_error_code` becomes non-zero.

2.  **Adaptive Application Behavior:**
    *   **Dynamic Resource Tuning:** An application can monitor `write_buffer_full_events`. If this value is consistently high, it may indicate that the initial buffer size is too small for the current workload, and the application could choose to re-create the conveyor with a larger `max_write_capacity`.
    *   **Load Shedding:** If write latency is consistently high, an application could implement a backpressure mechanism, temporarily reducing the rate of writes to allow the conveyor's background worker to catch up.

3.  **Debugging and Troubleshooting:**
    *   **Pinpointing Bottlenecks:** The metrics help distinguish between application-level performance and backend storage performance. High `avg_write_latency_ms`, for instance, strongly suggests a bottleneck in the underlying storage system, not in the application logic itself.
    *   **Diagnosing Silent Failures:** The `last_error_code` is invaluable for immediately detecting and diagnosing I/O errors that occur asynchronously in the background, which might otherwise go unnoticed until much later.

### C++17 Monitoring Example

The modern C++ interface makes it trivial to set up a simple monitoring thread:

```cpp
#include "libconveyor/conveyor_modern.hpp"
#include <iostream>
#include <thread>
#include <chrono>

void monitor_conveyor(libconveyor::v2::Conveyor& conveyor, std::atomic<bool>& stop) {
    while (!stop) {
        std::this_thread::sleep_for(std::chrono::seconds(5));
        
        auto stats = conveyor.stats();
        
        std::cout << "\n--- Conveyor Stats (last 5s) ---\n"
                  << "Bytes Written: " << stats.bytes_written << "\n"
                  << "Bytes Read: " << stats.bytes_read << "\n"
                  << "Avg Write Latency: " << stats.avg_write_latency.count() << " ms\n"
                  << "Avg Read Latency: " << stats.avg_read_latency.count() << " ms\n"
                  << "Write Buffer Full Events: " << stats.write_buffer_full_events << "\n"
                  << "Last Error: " << stats.last_error_code << "\n"
                  << "---------------------------------\n\n";
    }
}
```

## Performance

`libconveyor` is designed to provide significant performance gains by hiding I/O latency, especially when an application can perform other work while writes are happening asynchronously in the background.

### Write Benchmark

**Scenario:** Write 10MB of data in 4KB blocks with a simulated backend latency of 2ms per block.

| Benchmark                  | Total Time (ms) | Throughput (MB/s) | Avg Latency (us) | P99 Latency (us) |
| :------------------------- | :-------------- | :---------------- | :--------------- | :--------------- |
| **Raw POSIX Write** (Blocking) | 41518.9         | 0.240854          | 16218.1          | 16683.9          |
| **libconveyor Write** (Async)  | 1.4705          | 6800.41           | 0.544609         | 5.9              |

**Interpretation:** The "Raw POSIX Write" scenario is blocked waiting for each slow write operation. The "libconveyor" scenario measures only the time to enqueue the writes into memory, which is extremely fast. This demonstrates the library's ability to nearly eliminate perceived write latency. The adaptive buffer sizing ensures that the write buffer dynamically scales to handle bursts of data, further optimizing throughput without requiring manual tuning of buffer sizes.

### Read Benchmark

**Scenario:** Read 10MB of data in 4KB blocks with a 2ms simulated backend latency.

| Benchmark                      | Total Time (ms) | Throughput (MB/s) | Avg Latency (us) |
| :----------------------------- | :-------------- | :---------------- | :--------------- |
| **Raw POSIX Read** (Blocking)  | 41580.9         | 0.240495          | 16242.3          |
| **libconveyor Read** (Prefetching) | 46.591          | 214.634           | 18.1807          |

**Interpretation:** The `readWorker` thread proactively fetches data into the read-ahead cache. The application's read calls are then served instantly from this in-memory buffer, dramatically increasing throughput and reducing perceived latency. The adaptive read buffer intelligently grows when sequential access patterns are detected, optimizing prefetching for sustained high-speed reads.

## Testing

`libconveyor` is rigorously tested using a combination of unit, integration, and stress tests to ensure correctness, consistency, and thread-safety. The test suite is built using Google Test.

Our testing approach includes:

*   **Basic C API Tests (`conveyor_test.cpp`):** A suite of tests for the core C-style API, covering basic functionality, latency hiding, and error conditions.
*   **Modern C++ API Tests (`conveyor_modern_test.cpp`):** A parallel suite of tests for the modern C++17 wrapper, ensuring all features are correctly and safely exposed.
*   **Adaptive Buffer Tests (`adaptive_test.cpp`):** Dedicated tests for the adaptive buffer sizing logic, including scenarios that force buffer resizing while wrapped, and verification of read buffer growth heuristics.
*   **Stress Tests (`conveyor_stress_test.cpp`):** Tests designed to expose race conditions and complex bugs, including:
    *   Verifying read-after-write consistency under load.
    *   Ensuring the "generation counter" prevents stale reads after an `lseek`.
    *   Testing asynchronous error propagation.
    *   Verifying complex data snooping and overlap logic.
*   **Multi-threaded Application Stress Test (`conveyor_multi_thread_test.cpp`):** A dedicated stress test where multiple application threads concurrently read and write to the same conveyor instance to validate the thread-safety of the public API.
*   **Consistency Tests (`conveyor_consistency_test.cpp`):** Tests for specific data consistency scenarios, such as ensuring a `flush` correctly invalidates the read buffer.
*   **Partial I/O Tests (`conveyor_partial_io_test.cpp`):** Verifies that the library correctly handles partial reads and writes from the underlying storage.
*   **Illegal Operation Tests (`conveyor_illegal_op_test.cpp`):** Ensures the API fails gracefully and predictably when used incorrectly (e.g., writing to a read-only conveyor).
*   **Multi-Instance Tests (`conveyor_multi_instance_test.cpp`):** Checks for data corruption when multiple conveyor instances interact with the same file.

## Building and Testing

`libconveyor` uses CMake for its build system. It includes comprehensive unit tests, stress tests (using Google Test), and performance benchmarks.

**1. Configure CMake**
```bash
# Clone the repository and navigate into it
git clone <repository_url>
cd libconveyor

# Create a build directory
mkdir build
cd build

# Configure for your system (e.g., Visual Studio, Makefiles, Ninja)
# For Release build (recommended for performance testing)
cmake .. -DCMAKE_BUILD_TYPE=Release
```

**2. Build the Project**
```bash
# From the build directory
cmake --build . --config Release
```

**3. Run Tests**
```bash
# From the build directory

# Run all tests using CTest
ctest --build-config Release

# Or, run a specific test executable directly
# ./test/Release/conveyor_basic_test.exe
# ./test/Release/conveyor_modern_test.exe
# ./test/Release/conveyor_stress_test.exe
# ./test/Release/conveyor_multi_thread_test.exe
# ./test/Release/conveyor_consistency_test.exe
# ./test/Release/conveyor_partial_io_test.exe
# ./test/Release/conveyor_illegal_op_test.exe
# ./test/Release/conveyor_multi_instance_test.exe
# ./test/Release/adaptive_test.exe
```

**4. Run Benchmarks**
```bash
# From the build directory (use Release build for meaningful results)

# Run write benchmark
./benchmark/Release/conveyor_benchmark.exe

# Run read benchmark
./benchmark/Release/conveyor_read_benchmark.exe
```

## Contributing

Contributions are welcome! Please ensure that any new features or bug fixes include corresponding test cases and maintain the existing coding style.

## License

`libconveyor` is distributed under the **BSD 3-Clause License**. See the [LICENSE.md](./LICENSE.md) file for full details.
