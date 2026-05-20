#ifndef LIBCONVEYOR_SHM_CONVEYOR_H
#define LIBCONVEYOR_SHM_CONVEYOR_H

#include <stddef.h>
#include <stdint.h>
#include <pthread.h>

#define CONVEYOR_SOCKET_PATH "/tmp/conveyor.sock"
#define CONVEYOR_SHM_PREFIX "/conveyor_shm_"

namespace libconveyor {

struct ShmHeader {
    pthread_mutex_t mutex;
    pthread_cond_t  cond_producer;
    pthread_cond_t  cond_consumer;
    
    size_t capacity;
    size_t head;
    size_t tail;
    size_t size;
    
    bool eof;
    int last_error;
};

// Simple command structure for Unix Socket
enum CommandOp {
    CMD_REGISTER_WRITE = 1,
    CMD_REGISTER_READ  = 2,
    CMD_UNREGISTER     = 3
};

struct Command {
    CommandOp op;
    int system_fd;
    char path[512];
    size_t buffer_size;
};

struct Response {
    int status; // 0 for OK, negative for error
    char shm_name[64];
};

} // namespace libconveyor

#endif 
