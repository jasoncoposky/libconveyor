#ifndef LIBCONVEYOR_CONVEYOR_H
#define LIBCONVEYOR_CONVEYOR_H

#include <functional> // For std::function
#include <cstddef>    // For size_t

// MSVC specific definitions for ssize_t and off_t if they are not defined by other headers
#if defined(_MSC_VER)
#include <BaseTsd.h> // For SSIZE_T
#include <io.h>     // For _lseek, _read, _write
#include <fcntl.h>  // For _O_RDONLY, _O_WRONLY, _O_RDWR, _O_APPEND, _O_CREAT, _O_TRUNC
#define O_RDONLY _O_RDONLY
#define O_WRONLY _O_WRONLY
#define O_RDWR   _O_RDWR
#define O_APPEND _O_APPEND
#define O_CREAT  _O_CREAT
#define O_TRUNC  _O_TRUNC
#define S_IREAD  _S_IREAD
#define S_IWRITE _S_IWRITE
// Define O_ACCMODE for Windows if not present
#ifndef O_ACCMODE
#define O_ACCMODE (O_RDONLY | O_WRONLY | O_RDWR)
#endif
typedef long off_t;
typedef int ssize_t; // Windows doesn't have ssize_t, use int for signed size
#else
#include <sys/types.h> // For off_t, ssize_t on POSIX systems
#include <unistd.h> // For lseek, read, write
#include <fcntl.h>  // For O_RDONLY, O_WRONLY, O_RDWR, O_APPEND, O_CREAT, O_TRUNC, O_ACCMODE
#endif

#define LIBCONVEYOR_ERROR (-1)

typedef int storage_handle_t; // e.g., a file descriptor, or an iRODS file handle

#ifdef __cplusplus
extern "C" {
#endif

// Opaque handle to the conveyor object
typedef struct conveyor_t conveyor_t;

// Represents the underlying storage handle
// Callbacks for the library to interact with the real storage
typedef struct {
    ssize_t (*pwrite)(storage_handle_t, const void*, size_t, off_t);
    ssize_t (*pread)(storage_handle_t, void*, size_t, off_t);
    off_t   (*lseek)(storage_handle_t, off_t, int);
} storage_operations_t;

// Statistics structure for observability
typedef struct {
    // Volume in the last window
    size_t bytes_written;
    size_t bytes_read;

    // Latency in the last window (in milliseconds)
    size_t avg_write_latency_ms;
    size_t avg_read_latency_ms;

    // Congestion events in the last window
    size_t write_buffer_full_events;

    // Persistent sticky error code
    int last_error_code;
} conveyor_stats_t;

// --- API Functions ---

// Creates a conveyor instance with specified buffer sizes and open flags
conveyor_t* conveyor_create(
    storage_handle_t handle,
    int flags,
    const storage_operations_t* ops,
    size_t write_buffer_size,
    size_t read_buffer_size
);

// Destroys the conveyor, flushing any remaining data in the write buffer
void conveyor_destroy(conveyor_t* conv);

// POSIX-like I/O operations
ssize_t conveyor_write(conveyor_t* conv, const void* buf, size_t count);
ssize_t conveyor_read(conveyor_t* conv, void* buf, size_t count);
off_t conveyor_lseek(conveyor_t* conv, off_t offset, int whence);

// Forces a flush of the write-buffer to the underlying storage
int conveyor_flush(conveyor_t* conv);

// Retrieves the latest statistics, resetting the counters for the next window.
int conveyor_get_stats(conveyor_t* conv, conveyor_stats_t* stats);

} // End extern "C" block

// C++-only declarations
#ifdef __cplusplus
namespace libconveyor {
    struct RingBuffer; // Forward declaration
}
#endif

#endif // LIBCONVEYOR_CONVEYOR_H