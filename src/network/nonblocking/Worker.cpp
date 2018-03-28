#include "Worker.h"

#include <iostream>

#include <cstring>
#include <afina/execute/Command.h>
#include <signal.h>

#include "Utils.h"

namespace Afina {
namespace Network {
namespace NonBlocking {

// See Worker.h
Worker::Worker(std::shared_ptr<Afina::Storage> ps) noexcept : ps_(ps) {}

Worker::Worker(const Worker &other) noexcept : ps_(other.ps_) {}

// See Worker.h
void *Worker::RunOnRunProxy(void *proxy_args) noexcept {
    auto args = reinterpret_cast<Worker::ProxyArgs *>(proxy_args);
    try {
        args->worker_->thread_ = pthread_self();
        args->worker_->OnRun(args->socket_, args->fifo_read_path_, args->fifo_write_path_);
        delete args;
    } catch (std::exception &exception) {
        std::cerr << "Connection fails: " << exception.what() << std::endl;
    }

    return nullptr;
}

// See Worker.h
void Worker::Start(int server_socket, const std::string& fifo_read_path, const std::string& fifo_write_path) throw() {
    std::cout << "network debug: " << __PRETTY_FUNCTION__ << std::endl;
    running_.store(true);

    auto args = new Worker::ProxyArgs{
            .worker_ = this, .socket_ = server_socket,
            .fifo_read_path_ = fifo_read_path, .fifo_write_path_= fifo_write_path};
    pthread_t buffer;
    if (pthread_create(&buffer, nullptr, Worker::RunOnRunProxy, args) != 0) {
        throw std::runtime_error("pthread_create: " + std::string(strerror(errno)));
    }
}

// See Worker.h
void Worker::Stop() noexcept {
    std::cout << "network debug: " << __PRETTY_FUNCTION__ << std::endl;
    running_.store(false);
    manager_->Stop();
}

// See Worker.h
void Worker::Join() noexcept {
    std::cout << "network debug: " << __PRETTY_FUNCTION__ << std::endl;
    pthread_join(thread_, nullptr);
}

// See Worker.h
void Worker::OnRun(int server_socket, const std::string& fifo_read_path, const std::string& fifo_write_path) noexcept {
    std::cout << "network debug: " << __PRETTY_FUNCTION__ << std::endl;

    // TODO: implementation here
    // 1. Create epoll_context here
    // 2. Add server_socket to context
    // 3. Accept new connections, don't forget to call make_socket_nonblocking on
    //    the client socket descriptor
    // 4. Add connections to the local context
    // 5. Process connection events
    //
    // Do not forget to use EPOLLEXCLUSIVE flag when register socket
    // for events to avoid thundering herd type behavior.

    try {
        manager_ = new EpollManager(server_socket, fifo_read_path, fifo_write_path, this);
    } catch (std::exception &exception) {
        std::cout << exception.what() << std::endl;
        running_.store(false);
        return;
    }
    while (running_.load()) {
        try {
            manager_->WaitEvent();
        } catch (std::exception &exception) {
            std::cout << exception.what() << std::endl;

            delete manager_;
            running_.store(false);

            return;
        }
    }
}


} // namespace NonBlocking
} // namespace Network
} // namespace Afina
