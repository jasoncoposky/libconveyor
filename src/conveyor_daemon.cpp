#include "libconveyor/shm_conveyor.h"
#include "libconveyor/conveyor.h"
#include <sys/socket.h>
#include <sys/un.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <fcntl.h>
#include <unistd.h>
#include <iostream>
#include <vector>
#include <thread>
#include <map>
#include <mutex>
#include <cstring>
#include <cstdarg>

using namespace libconveyor;

static FILE* g_daemon_log = nullptr;
void daemon_log(const char* fmt, ...) {
    if (!g_daemon_log) {
        g_daemon_log = fopen("/tmp/daemon.log", "a");
        if (g_daemon_log) setvbuf(g_daemon_log, NULL, _IONBF, 0);
    }
    if (!g_daemon_log) return;
    va_list args;
    va_start(args, fmt);
    vfprintf(g_daemon_log, fmt, args);
    va_end(args);
}

struct Worker {
    conveyor_t* conv;
    int system_fd;
    std::string path;
    ShmHeader* shm_header;
    char* shm_data;
    size_t shm_total_size;
    std::thread thread;
    bool stop = false;
};

std::map<std::string, std::unique_ptr<Worker>> g_workers;
std::mutex g_workers_mutex;

// --- DAEMON-SIDE STORAGE CALLBACKS ---
extern "C" {
    ssize_t daemon_storage_pwrite(storage_handle_t h, const void* buf, size_t count, off_t offset) {
        int fd = *(int*)h;
        ssize_t ret = pwrite(fd, buf, count, offset);
        if (ret < 0) daemon_log("daemon: pwrite failed fd=%d error=%d\n", fd, errno);
        return ret;
    }
    ssize_t daemon_storage_pread(storage_handle_t h, void* buf, size_t count, off_t offset) {
        int fd = *(int*)h;
        return pread(fd, buf, count, offset);
    }
    off_t daemon_storage_lseek(storage_handle_t h, off_t offset, int whence) {
        int fd = *(int*)h;
        return lseek(fd, offset, whence);
    }
}

void daemon_worker_loop(Worker* w) {
    daemon_log("daemon: worker loop starting for %s\n", w->path.c_str());
    while (!w->stop) {
        pthread_mutex_lock(&w->shm_header->mutex);
        while (w->shm_header->size == 0 && !w->stop) {
            struct timespec ts;
            clock_gettime(CLOCK_REALTIME, &ts);
            ts.tv_sec += 1;
            int ret = pthread_cond_timedwait(&w->shm_header->cond_consumer, &w->shm_header->mutex, &ts);
            if (ret == ETIMEDOUT) {
                // Periodically log to confirm the worker is alive
            }
            msync(w->shm_header, w->shm_total_size, MS_SYNC);
        }
        if (w->stop) { pthread_mutex_unlock(&w->shm_header->mutex); break; }
        if (w->shm_header->size == 0) { pthread_mutex_unlock(&w->shm_header->mutex); continue; }

        size_t count = w->shm_header->size;
        daemon_log("daemon: worker %s processing %zu bytes\n", w->path.c_str(), count);
        size_t tail = w->shm_header->tail;
        size_t first_chunk = std::min(count, w->shm_header->capacity - tail);
        
        pthread_mutex_unlock(&w->shm_header->mutex);
        
        // Feed into the asynchronous conveyor for background persistence
        ssize_t written = conveyor_write(w->conv, w->shm_data + tail, first_chunk);
        
        pthread_mutex_lock(&w->shm_header->mutex);
        if (written > 0) {
            w->shm_header->tail = (w->shm_header->tail + (size_t)written) % w->shm_header->capacity;
            w->shm_header->size -= (size_t)written;
            pthread_cond_signal(&w->shm_header->cond_producer);
        } else if (written < 0) {
            daemon_log("daemon: conveyor_write failed for %s return %zd errno=%d\n", w->path.c_str(), written, errno);
            w->shm_header->last_error = (int)written;
        }
        pthread_mutex_unlock(&w->shm_header->mutex);
    }
    daemon_log("daemon: worker loop exiting for %s\n", w->path.c_str());
}

int main() {
    unlink(CONVEYOR_SOCKET_PATH);
    int server_fd = socket(AF_UNIX, SOCK_STREAM, 0);
    struct sockaddr_un addr;
    memset(&addr, 0, sizeof(addr));
    addr.sun_family = AF_UNIX;
    strncpy(addr.sun_path, CONVEYOR_SOCKET_PATH, sizeof(addr.sun_path)-1);
    
    if (bind(server_fd, (struct sockaddr*)&addr, sizeof(addr)) < 0) {
        perror("daemon: bind failed");
        return 1;
    }
    listen(server_fd, 64);
    
    daemon_log("conveyor_daemon: listening on %s\n", CONVEYOR_SOCKET_PATH);
    
    while (true) {
        int client_fd = accept(server_fd, NULL, NULL);
        if (client_fd < 0) continue;
        daemon_log("daemon: accept client fd=%d\n", client_fd);

        Command cmd;
        memset(&cmd, 0, sizeof(cmd));
        struct msghdr msg = {0};
        struct iovec iov[1];
        iov[0].iov_base = (void*)&cmd;
        iov[0].iov_len = sizeof(cmd);
        msg.msg_iov = iov;
        msg.msg_iovlen = 1;
        
        union { struct cmsghdr cm; char control[CMSG_SPACE(sizeof(int))]; } control_un;
        msg.msg_control = control_un.control;
        msg.msg_controllen = sizeof(control_un.control);
        
        ssize_t n = recvmsg(client_fd, &msg, 0);
        if (n <= 0) { daemon_log("daemon: recvmsg failed n=%zd\n", n); close(client_fd); continue; }
        
        struct cmsghdr* cmsg = CMSG_FIRSTHDR(&msg);
        int passed_fd = -1;
        if (cmsg && cmsg->cmsg_level == SOL_SOCKET && cmsg->cmsg_type == SCM_RIGHTS) {
            passed_fd = *((int*)CMSG_DATA(cmsg));
            daemon_log("daemon: received passed fd=%d\n", passed_fd);
        }
        
        Response resp = {0};
        std::string path(cmd.path);
        daemon_log("daemon: op=%d path=%s buff_size=%zu\n", cmd.op, path.c_str(), cmd.buffer_size);
        
        if (cmd.op == CMD_REGISTER_WRITE) {
            std::lock_guard<std::mutex> lock(g_workers_mutex);
            std::string shm_name = CONVEYOR_SHM_PREFIX + std::to_string(std::hash<std::string>{}(path));
            
            if (g_workers.count(path)) {
                daemon_log("daemon: re-mapping existing worker for %s\n", path.c_str());
                if (passed_fd >= 0) {
                    close(g_workers[path]->system_fd);
                    g_workers[path]->system_fd = passed_fd;
                }
                resp.status = 0;
                strncpy(resp.shm_name, shm_name.c_str(), 63);
            } else {
                daemon_log("daemon: initializing new worker for %s\n", path.c_str());
                int shm_fd = shm_open(shm_name.c_str(), O_CREAT | O_RDWR, 0666);
                size_t full_size = sizeof(ShmHeader) + cmd.buffer_size;
                ftruncate(shm_fd, full_size);
                void* ptr = mmap(NULL, full_size, PROT_READ | O_RDWR, MAP_SHARED, shm_fd, 0);
                close(shm_fd);

                ShmHeader* header = (ShmHeader*)ptr;
                memset(header, 0, sizeof(ShmHeader));
                header->capacity = cmd.buffer_size;
                
                pthread_mutexattr_t m_attr;
                pthread_mutexattr_init(&m_attr);
                pthread_mutexattr_setpshared(&m_attr, PTHREAD_PROCESS_SHARED);
                pthread_mutex_init(&header->mutex, &m_attr);
                
                pthread_condattr_t c_attr;
                pthread_condattr_init(&c_attr);
                pthread_condattr_setpshared(&c_attr, PTHREAD_PROCESS_SHARED);
                pthread_cond_init(&header->cond_producer, &c_attr);
                pthread_cond_init(&header->cond_consumer, &c_attr);
                
                auto w = std::make_unique<Worker>();
                w->system_fd = passed_fd;
                w->path = path;
                w->shm_header = header;
                w->shm_data = (char*)ptr + sizeof(ShmHeader);
                w->shm_total_size = full_size;
                
                conveyor_config_t c_cfg = {0};
                c_cfg.handle = &w->system_fd;
                c_cfg.ops = {daemon_storage_pwrite, daemon_storage_pread, daemon_storage_lseek};
                c_cfg.flags = O_RDWR;
                c_cfg.initial_write_size = 32 * 1024 * 1024; 
                c_cfg.initial_read_size = 0;
                c_cfg.max_write_size = 128 * 1024 * 1024;
                w->conv = conveyor_create(&c_cfg);

                w->thread = std::thread(daemon_worker_loop, w.get());
                g_workers[path] = std::move(w);
                
                resp.status = 0;
                strncpy(resp.shm_name, shm_name.c_str(), 63);
            }
        } else if (cmd.op == CMD_UNREGISTER) {
            std::lock_guard<std::mutex> lock(g_workers_mutex);
            if (g_workers.count(path)) {
                daemon_log("daemon: teardown for %s\n", path.c_str());
                auto& w = g_workers[path];
                w->stop = true;
                pthread_cond_broadcast(&w->shm_header->cond_consumer);
                if (w->thread.joinable()) w->thread.join();
                conveyor_destroy(w->conv);
                munmap(w->shm_header, w->shm_total_size);
                shm_unlink((CONVEYOR_SHM_PREFIX + std::to_string(std::hash<std::string>{}(path))).c_str());
                close(w->system_fd);
                g_workers.erase(path);
            }
            resp.status = 0;
        }
        
        send(client_fd, &resp, sizeof(resp), 0);
        close(client_fd);
    }
    return 0;
}
