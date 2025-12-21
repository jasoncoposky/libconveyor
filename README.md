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
    
    // Create a conveyor
    conveyor_t* conv = conveyor_create(
        nullptr, // dummy handle
        O_RDWR, 
        &my_storage_ops,
        1024 * 1024, // 1MB write buffer
        512 * 1024   // 512KB read buffer
    );

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

## Performance

`libconveyor` is designed to provide significant performance gains by hiding I/O latency, especially when an application can perform other work while writes are happening asynchronously in the background.

### Write Benchmark

**Scenario:** Write 10MB of data in 4KB blocks with a simulated backend latency of ~15ms per block.

| Benchmark                  | Total Time (ms) | Throughput (MB/s) | Avg Latency (us) |
| :------------------------- | :-------------- | :---------------- | :--------------- |
| **Raw POSIX Write** (Blocking) | 36739.7         | 0.272             | 14351.2          |
| **libconveyor Write** (Async)  | 1.8625          | 5369.13           | 0.700            |

**Interpretation:** The "Raw POSIX Write" scenario is blocked waiting for each slow write operation. The "libconveyor" scenario measures only the time to enqueue the writes into memory, which is extremely fast. This demonstrates the library's ability to nearly eliminate perceived write latency.

### Read Benchmark

**Scenario:** Read 10MB of data in 4KB blocks with a 2ms simulated backend latency.

| Benchmark                      | Total Time (ms) | Throughput (MB/s) | Avg Latency (us) |
| :----------------------------- | :-------------- | :---------------- | :--------------- |
| **Raw POSIX Read** (Blocking)  | 7095.91         | 1.40926           | 2768.12          |
| **libconveyor Read** (Prefetching) | 12.3602         | 809.048           | 4.72941          |

**Interpretation:** The `readWorker` thread proactively fetches data into the read-ahead cache. The application's read calls are then served instantly from this in-memory buffer, dramatically increasing throughput and reducing perceived latency.

## Testing

`libconveyor` is rigorously tested using a combination of unit, integration, and stress tests to ensure correctness, consistency, and thread-safety. The test suite is built using Google Test.

Our testing approach includes:

*   **Basic C API Tests (`conveyor_test.cpp`):** A suite of tests for the core C-style API, covering basic functionality, latency hiding, and error conditions.
*   **Modern C++ API Tests (`conveyor_modern_test.cpp`):** A parallel suite of tests for the modern C++17 wrapper, ensuring all features are correctly and safely exposed.
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
cmake --build .
```

**3. Run Tests**
```bash
# From the build directory

# Run all tests using CTest
ctest

# Or, run a specific test executable directly
# ./test/conveyor_basic_test
# ./test/conveyor_modern_test
# ./test/conveyor_stress_test
# ./test/conveyor_multi_thread_test
# ./test/conveyor_consistency_test
# ./test/conveyor_partial_io_test
# ./test/conveyor_illegal_op_test
# ./test/conveyor_multi_instance_test
```

**4. Run Benchmarks**
```bash
# From the build directory (use Release build for meaningful results)

# Run write benchmark
./benchmark/conveyor_benchmark

# Run read benchmark
./benchmark/conveyor_read_benchmark
```

## Contributing

Contributions are welcome! Please ensure that any new features or bug fixes include corresponding test cases and maintain the existing coding style.

## License

`libconveyor` is distributed under the **BSD 3-Clause License**. See the [LICENSE.md](./LICENSE.md) file for full details.