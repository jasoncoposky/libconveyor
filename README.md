<p align="center">
  <img src="assets/libconveyor_logo.jpg" alt="libconveyor Logo" width="500"/>
</p>

# libconveyor: High-Performance I/O Buffering Library

[![License](https://img.shields.io/badge/License-BSD_3--Clause-blue.svg)](https://opensource.org/licenses/BSD-3-Clause)

`libconveyor` is a cross-platform C++20 library designed to significantly accelerate I/O operations by implementing a highly efficient, asynchronous, and adaptive buffering layer between the application and the underlying storage system.

## Key Features

- **Extreme Handoff (1000x Speedup)**: Achieve spectacular I/O acceleration by decoupling application threads from slow backend storage.
- **Zero-Copy Segment Pool**: High-performance API for direct ownership handoff of pre-allocated, hardware-aligned memory segments.
- **Lock-Free Atomic Reservation**: Uses atomic cursors to allow multiple threads to submit data in parallel without central mutex contention.
- **Compute Foundation (Citor)**: Leverages the **citor** high-performance thread pool for sub-microsecond task dispatch and hardware-topology awareness.
- **Wait-Free Metadata Queue**: Integrated **moodycamel::ConcurrentQueue** for zero-latency task handoff between producers and workers.
- **Eager Prefaulted Headroom**: Pre-allocates and prefaults up to 8GB of Segment Pool to eliminate "Page Fault Storms" on the hot path.
- **Adaptive Buffer Management**: Automatically scales I/O buffers from 32MB up to 2GB based on workload demand and latency.
- **Read-Ahead Prefetching**: Proactively fetches massive data chunks (configurable up to 128MB+) to serve reads at RAM speeds.
- **Unified C/C++ API**: Provides a stable C-style interface for easy integration into legacy and modern systems.

## Performance & Scalability

`libconveyor` is designed for extreme throughput on high-latency links (e.g. Cloud/S3, Remote NAS, Satellite).

### High-Latency Burst Benchmark
*Simulated Backend Latency: 500 ms*
*Data Volume: 1GB*

| Metric | Synchronous POSIX | libconveyor (Extreme) | **Speedup** |
| :--- | :--- | :--- | :--- |
| **Submission Throughput** | 0.12 MB/s | **GB/s (Instant)** | **~1,000x+** |
| **Read Throughput** | 0.12 MB/s | **125 GB/s (Hot)** | **~1,000,000x** |

### Extreme Scalability (2,000 Simultaneous Streams)
| Foundation | Total Time | Throughput | Result |
| :--- | :--- | :--- | :--- |
| **Dedicated Threads** | 14.9s | 16.7 MB/s | System Unstable |
| **Citor Tasks** | **2.5s** | **97.8 MB/s** | **~6x Speedup** |

---

## Installation & Usage

### 1. Build
```bash
mkdir build && cd build
cmake .. -DCMAKE_BUILD_TYPE=Release
make -j$(nproc)
```

### 2. Zero-Copy Usage (Maximum Performance)
```c
#include "libconveyor/conveyor.h"

size_t size = 0;
void* buf = conveyor_get_buffer(conv, &size);
// Write data directly into buf...
conveyor_submit_buffer(conv, buf, actual_count, file_offset);
```

### 3. POSIX-like Usage
```c
conveyor_write(conv, application_ptr, data_len);
```

Detailed API documentation is available in [API.md](API.md).

## License

`libconveyor` is distributed under the **BSD 3-Clause License**. See the [LICENSE.md](./LICENSE.md) file for full details.
