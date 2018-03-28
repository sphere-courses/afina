#include "ServerImpl.h"

#include <cassert>
#include <cstring>
#include <iostream>
#include <memory>
#include <pthread.h>
#include <stdexcept>

#include <arpa/inet.h>
#include <netdb.h>
#include <netinet/in.h>
#include <signal.h>
#include <sys/socket.h>
#include <sys/types.h>
#include <unistd.h>

#include <afina/Storage.h>
#include <bits/signum.h>
#include <cstdint>
#include <fcntl.h>
#include <sys/stat.h>

#include "Utils.h"
#include "Worker.h"

namespace Afina {
namespace Network {
namespace NonBlocking {

// See Server.h
ServerImpl::ServerImpl(std::shared_ptr<Afina::Storage> ps) noexcept : Server(ps) {}

// See ServerImpl.h
void ServerImpl::AssignFifo(const std::string& fifo_read_file, const std::string& fifo_write_file) noexcept {
    fifo_read_file_ = fifo_read_file;
    fifo_write_file_ = fifo_write_file;
}

// See Server.h
void ServerImpl::Start(uint16_t port, uint16_t n_workers) throw() {
    std::cout << "network debug: " << __PRETTY_FUNCTION__ << std::endl;

    listen_port_ = port;

    // If a client closes a connection, this will generally produce a SIGPIPE
    // signal that will kill the process. We want to ignore this signal, so send()
    // just returns -1 when this happens.
    sigset_t sig_mask;
    sigemptyset(&sig_mask);
    sigaddset(&sig_mask, SIGPIPE);
    if (pthread_sigmask(SIG_BLOCK, &sig_mask, nullptr) != 0) {
        throw std::runtime_error("Unable to mask SIGPIPE");
    }

    // Create server socket
    struct sockaddr_in server_addr;
    std::memset(&server_addr, 0, sizeof(server_addr));
    server_addr.sin_family = AF_INET;               // IPv4
    server_addr.sin_port = htons(listen_port_);     // TCP port number
    server_addr.sin_addr.s_addr = INADDR_ANY;       // Bind to any address

    server_socket_ = socket(PF_INET, SOCK_STREAM, IPPROTO_TCP);
    if (server_socket_ == -1) {
        throw std::runtime_error("Failed to open socket");
    }

    int opts = 1;
    // SO_REUSEADDR - in order to use epoll technique
    if (setsockopt(server_socket_, SOL_SOCKET, SO_REUSEADDR, &opts, sizeof(opts)) == -1) {
        close(server_socket_);
        throw std::runtime_error("Socket setsockopt() failed");
    }

    if (bind(server_socket_, (struct sockaddr *)&server_addr, sizeof(server_addr)) == -1) {
        close(server_socket_);
        throw std::runtime_error("Socket bind() failed");
    }

    make_socket_non_blocking(server_socket_);
    if (listen(server_socket_, 5) == -1) {
        close(server_socket_);
        throw std::runtime_error("Socket listen() failed");
    }

//    // Create read fifo file if necessary
    int fifo_read_fd = -1;
    if(fifo_read_file_.length()){
        if(mkfifo(fifo_read_file_.c_str(), 0666) == -1){
            close(server_socket_);
            throw std::runtime_error("Fifo file mkfifo() failed. " + std::string(strerror(errno)));
        }

        if((fifo_read_fd = open(fifo_read_file_.c_str(), O_NONBLOCK | O_RDONLY)) == -1){
            close(server_socket_);
            throw std::runtime_error("Fifo file open() failed. " + std::string(strerror(errno)));
        }

        make_socket_non_blocking(fifo_read_fd);
    }

//    // Create write fifo file if necessary
      int fifo_write_fd = -1;
    if(fifo_write_file_.length()){

        if(mkfifo(fifo_write_file_.c_str(), 0666) == -1){
            close(server_socket_);
            throw std::runtime_error("Fifo file mkfifo() failed. " + std::string(strerror(errno)));
        }

        // Non blocking variant is forbidden, process must wait
        // until reader will connect to the other side of fifo file
        if((fifo_write_fd = open(fifo_write_file_.c_str(), O_WRONLY)) == -1){
            close(server_socket_);
            throw std::runtime_error("Fifo file open() failed. " + std::string(strerror(errno)));
        }

        make_socket_non_blocking(fifo_write_fd);
    }


    for (int i = 0; i < n_workers; i++) {
        auto new_worker = workers_.insert(new Worker(pStorage));

        // TODO: Prepare more than one pair of read-write fifo files (divide pairs between workers)
        // Assign fifo files to only one worker
        (*new_worker.first)->Start(server_socket_, (i == 0 ? fifo_read_fd : -1), (i == 0 ? fifo_write_fd : -1));
    }
}

// See Server.h
void ServerImpl::Stop() noexcept {
    std::cout << "network debug: " << __PRETTY_FUNCTION__ << std::endl;

    close(server_socket_);

    for (auto &worker : workers_) {
        if(worker->running_.load()) {
            worker->Stop();
        }
    }
}

// See Server.h
void ServerImpl::Join() noexcept {
    std::cout << "network debug: " << __PRETTY_FUNCTION__ << std::endl;
    for (auto &worker : workers_) {
        if(worker->running_.load()) {
            worker->Join();
            delete worker;
        }
    }
    unlink(fifo_read_file_.c_str());
    unlink(fifo_write_file_.c_str());
}

} // namespace NonBlocking
} // namespace Network
} // namespace Afina
