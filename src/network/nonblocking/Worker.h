#ifndef AFINA_NETWORK_NONBLOCKING_WORKER_H
#define AFINA_NETWORK_NONBLOCKING_WORKER_H

#include <memory>
#include <queue>
#include <unordered_map>
#include <unordered_set>
#include <pthread.h>
#include <atomic>
#include <set>
#include <sys/epoll.h>
#include <protocol/Parser.h>
#include <sstream>
#include <cstdint>

#include "EpollManager.h"

namespace Afina {

// Forward declaration, see afina/Storage.h
class Storage;

namespace Network {
namespace NonBlocking {

// Forward declaration, see EpollManager.h
class EpollManager;

/**
 * # Thread running epoll
 * On Start spaws background thread that is doing epoll on the given server
 * socket and process incoming connections and its data
 */
class Worker {
public:
    friend EpollManager;

    explicit Worker(std::shared_ptr<Afina::Storage> ps) noexcept ;

    Worker(const Worker &other) noexcept ;

    ~Worker() noexcept = default;

    /**
     * Spaws new background thread that is doing epoll on the given server
     * socket. Once connection accepted it must be registered and being processed
     * on this thread
     */
    void Start(int server_socket, int fifo_read_fd = -1, int fifo_write_fd = -1) throw() ;

    /**
     * Signal background thread to stop. After that signal thread must stop to
     * accept new connections and must stop read new commands from existing. Once
     * all readed commands are executed and results are send back to client, thread
     * must stop
     */
    void Stop() noexcept ;

    /**
     * Blocks calling thread until background one for this worker is actually
     * been destoryed
     */
    void Join() noexcept ;

    std::atomic<bool> running_;

protected:
    /**
     * Method executing by background thread
     */
    void OnRun(int server_socket, int fifo_read_fd, int fifo_write_fd) noexcept ;

private:
    static void *RunOnRunProxy(void *proxy_args) noexcept ;


    // Nested class to pass parameters of new connection through proxy function
    class ProxyArgs {
    public:
        Worker *worker_;
        int socket_;
        int fifo_read_fd_, fifo_write_fd_;
    };

    pthread_t thread_;
    std::shared_ptr<Afina::Storage> ps_;
    EpollManager *manager_;
};

} // namespace NonBlocking
} // namespace Network
} // namespace Afina
#endif // AFINA_NETWORK_NONBLOCKING_WORKER_H
