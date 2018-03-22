#include "Worker.h"

#include <iostream>

#include <unistd.h>
#include <sys/socket.h>
#include <cstring>
#include <afina/execute/Command.h>

#include "Utils.h"

namespace Afina {
namespace Network {
namespace NonBlocking {

char Worker::EpollManager::Connection::addition_[] = "\r\n";
size_t Worker::EpollManager::Connection::addition_len_ = sizeof(addition_) - 1;


// See Worker.h
Worker::Worker(std::shared_ptr<Afina::Storage> ps) : ps_(ps) {}

Worker::Worker(const Worker& other) : ps_(other.ps_) {}

// See Worker.h
void * Worker::RunOnRunProxy(void *proxy_args){
    auto args = reinterpret_cast<Worker::ProxyArgs *>(proxy_args);
    try{
        args->worker_->thread_ = pthread_self();
        args->worker_->OnRun(args->socket_);
        delete args;
    } catch (std::runtime_error &exception) {
        std::cerr << "Connection fails: " << exception.what() << std::endl;
    }
    return nullptr;
}

// See Worker.h
void Worker::Start(int server_socket) {
    std::cout << "network debug: " << __PRETTY_FUNCTION__ << std::endl;
    running_.store(true);

    auto args = new ProxyArgs{.worker_ = this, .socket_ = server_socket};
    pthread_t buffer;
    if(pthread_create(&buffer, nullptr, Worker::RunOnRunProxy, args) != 0){
        throw std::runtime_error("pthread_create: " + std::string(strerror(errno)));
    }
}

// See Worker.h
void Worker::Stop() {
    std::cout << "network debug: " << __PRETTY_FUNCTION__ << std::endl;
    running_.store(false);
    manager_->Stop();
}

// See Worker.h
void Worker::Join() {
    std::cout << "network debug: " << __PRETTY_FUNCTION__ << std::endl;
    pthread_join(thread_, nullptr);
}

// See Worker.h
void Worker::OnRun(int server_socket) {
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
        manager_ = new EpollManager(server_socket, this);
    } catch (std::exception & exception) {
        std::cout << exception.what() << std::endl;
        pthread_exit(nullptr);
    }
    while (running_.load()) {
        try {
            manager_->WaitEvent();
        } catch (std::exception& exception){
            std::cout << exception.what() << std::endl;
            pthread_exit(nullptr);
        }
    }
}


// See Worker.h
Worker::EpollManager::EpollManager(int server_socket, Worker *worker) : server_socket_(server_socket), worker_{worker} {
    std::cout << "network debug: " << __PRETTY_FUNCTION__;

    // Since UNIX kernel 2.6.7 it is okay to pass any positive size_t argument
    // to epoll_create with no difference
    if((epoll_fd_ = epoll_create(1)) == -1){
        throw std::runtime_error("epoll_create: " + std::string(strerror(errno)));
    }
    // EPOLLERR, EPOLLHUP - are always monitored
    epoll_event server_event;
    server_event.events = EpollManager::EPOLLFLAG::EPOLLEXCLUSIVE | EPOLLIN;
    server_event.data.fd = server_socket;

    if(epoll_ctl(epoll_fd_, EPOLL_CTL_ADD, server_socket, &server_event) == -1){
        throw std::runtime_error("epoll_ctl: " + std::string(strerror(errno)));
    }
}

// See Worker.h
Worker::EpollManager::~EpollManager() {
    std::cout << "network debug: " << __PRETTY_FUNCTION__ << std::endl;

    close_socket(server_socket_);
    for(auto socket : connection_sockets_){
        close_socket(socket);
    }
}

// See Worker.h
void Worker::EpollManager::Stop() {
    close(epoll_fd_);
}

// See Worker.h
void Worker::EpollManager::WaitEvent() {
    std::cout << "network debug: " << __PRETTY_FUNCTION__ << std::endl;

    int events_num;
    int event_socket;

    if((events_num = epoll_wait(epoll_fd_, events_, max_events_, max_timeout_)) == -1){
        throw std::runtime_error("epoll_wait: " + std::string(strerror(errno)));
    }
    for(int i = 0; i < events_num; ++i){
        event_socket = events_[i].data.fd;
        if(event_socket == server_socket_){
            if(events_[i].events & EPOLLERR){
                std::cout << "Error happened on server socket. Client sockets are monitored" << std::endl;
                TerminateEvent(server_socket_, true);
            } else if(events_[i].events & EPOLLHUP) {
                std::cout << "Server socket closed connection. Client sockets are monitored" << std::endl;
                TerminateEvent(server_socket_, true);
            } else if(events_[i].events & EPOLLIN){
                AcceptEvent();
            } else {
                //TODO: is server socket valid after signaled unknown event
                std::cout << "Unknown event happened on server socket. Client sockets are monitored" << std::endl;
            }
        } else {
            if(events_[i].events & EPOLLERR){
                std::cout << "Error happened on client socket" << std::endl;
                TerminateEvent(event_socket);
            } else if(events_[i].events & EPOLLHUP) {
                std::cout << "Client socket close connection" << std::endl;
                TerminateEvent(event_socket);
            } else {
                if(connection_sockets_.count(event_socket) != 0 &&
                   events_[i].events & EPOLLIN) {
                    ReadEvent(event_socket);
                    //continue;
                }
                if(connection_sockets_.count(event_socket) != 0 &&
                   events_[i].events & EPOLLOUT) {
                    WriteEvent(event_socket);
                    //continue;
                }
                if(!(events_[i].events & EPOLLIN) && !(events_[i].events & EPOLLOUT)){
                    //TODO: is client socket valid after signaled unknown event
                    std::cout << "Unknown event happened on client socket" << std::endl;
                    //continue;
                }
            }
        }
    }
}

// See Worker.h
void Worker::EpollManager::AcceptEvent() {
    std::cout << "network debug: " << __PRETTY_FUNCTION__;

    int new_socket;
    if((new_socket = accept(server_socket_, nullptr, nullptr)) == -1){
        TerminateEvent(server_socket_, true);
        //TODO: can we continue EpollManager?
        throw std::runtime_error("server_socket: " + std::string(strerror(errno)));
    }

    make_socket_non_blocking(new_socket);

    connection_sockets_.insert(new_socket);
    connections_[new_socket] = Connection();

    epoll_event client_event;
    client_event.events  = EpollManager::EPOLLFLAG::EPOLLEXCLUSIVE | EPOLLIN | EPOLLOUT;
    client_event.data.fd = new_socket;

    if(epoll_ctl(epoll_fd_, EPOLL_CTL_ADD, new_socket, &client_event) == -1){
        throw std::runtime_error("epoll_ctl: " + std::string(strerror(errno)));
    }
}

// See Worker.h
void Worker::EpollManager::TerminateEvent(int socket, bool is_server){
    std::cout << "network debug: " << __PRETTY_FUNCTION__ << std::endl;

    if(!is_server) {
        connection_sockets_.erase(socket);
        connections_.erase(socket);
    }
    // Warning! Set event to nullptr is error before kernel 2.6.9
    if(epoll_ctl(epoll_fd_, EPOLL_CTL_DEL, socket, nullptr) == -1){
        throw std::runtime_error("epoll_ctl: " + std::string(strerror(errno)));
    }
    close_socket(socket);
}

// See Worker.h
void Worker::EpollManager::ReadEvent(int socket) {
    std::cout << "network debug: " << __PRETTY_FUNCTION__ <<  std::endl;

    Connection& event_connection = connections_[socket];
    if (event_connection.state_ != Connection::State::BLOCKON_RADD &&
        event_connection.state_ != Connection::State::BLOCKON_RDATA &&
        event_connection.state_ != Connection::State::BLOCKON_RCOM &&
        event_connection.state_ != Connection::State::BLOCKON_NONE) {
        throw std::runtime_error("Connection in inconsistent state");
    }

    if (!worker_->running_.load()) {
        TerminateEvent(socket);
        return;
    }

    try {
        if (event_connection.state_ == Connection::State::BLOCKON_NONE ||
            event_connection.state_ == Connection::State::BLOCKON_RCOM) {
            event_connection.parsed_now_ = 0;
            event_connection.read_now_ = 0;
            while (!event_connection.parser_.Parse(event_connection.buffer_ + event_connection.parsed_,
                                                   event_connection.current_buffer_size_ - event_connection.parsed_,
                                                   event_connection.parsed_now_)) {
                event_connection.parsed_ += event_connection.parsed_now_;

                if (event_connection.state_ == Connection::State::BLOCKON_NONE) {
                    if (event_connection.current_buffer_size_ == event_connection.max_buffer_size_) {
                        event_connection.parsed_ = 0;
                        event_connection.current_buffer_size_ = 0;
                    }
                }

                event_connection.read_now_ = recv(socket,
                                                  event_connection.buffer_ + event_connection.current_buffer_size_,
                                                  event_connection.max_buffer_size_ -
                                                  event_connection.current_buffer_size_,
                                                  0);

                if (event_connection.read_now_ == -1) {
                    if (errno == EAGAIN || errno == EWOULDBLOCK) {
                        event_connection.state_ = Connection::State::BLOCKON_RCOM;
                        return;
                    }
                    std::cout << "recv error: " << std::string(strerror(errno)) << std::endl;
                    TerminateEvent(socket);
                    return;
                } else if (event_connection.read_now_ == 0) {
                    TerminateEvent(socket);
                    return;
                } else {
                    event_connection.current_buffer_size_ += event_connection.read_now_;
                }
            }
            event_connection.parsed_ += event_connection.parsed_now_;


            event_connection.command_ = std::move(
                    std::shared_ptr<Execute::Command>(event_connection.parser_.Build(event_connection.body_size_)));
            if (event_connection.body_size_ > event_connection.max_data_size_) {
                throw std::runtime_error("Too long data_block");
            } else if (event_connection.body_size_ == 0) {
                event_connection.current_buffer_size_ =
                        event_connection.current_buffer_size_ - event_connection.parsed_;
                memmove(event_connection.buffer_,
                        event_connection.buffer_ + event_connection.parsed_,
                        event_connection.current_buffer_size_);
                event_connection.parsed_ = 0;
                event_connection.state_ = Connection::State::BLOCKON_NONE;
            } else {
                event_connection.current_data_size_ = std::min(
                        event_connection.current_buffer_size_ - event_connection.parsed_,
                        static_cast<size_t >(event_connection.body_size_));

                event_connection.current_buffer_size_ =
                        event_connection.current_buffer_size_ - event_connection.parsed_ -
                        event_connection.current_data_size_;

                std::memcpy(event_connection.data_block_,
                            event_connection.buffer_ + event_connection.parsed_,
                            event_connection.current_data_size_);

                std::memmove(event_connection.buffer_,
                             event_connection.buffer_ + event_connection.parsed_ + event_connection.current_data_size_,
                             event_connection.current_buffer_size_);


                event_connection.state_ = Connection::State::BLOCKON_RDATA;
                // Continue in Connection::State::BLOCKON_RDATA
            }
        }
        if (event_connection.state_ == Connection::State::BLOCKON_RDATA) {

            event_connection.read_now_ = ReadStrict_NonBlock(socket, event_connection.data_block_ +
                                                                     event_connection.current_data_size_,
                                                             event_connection.body_size_ -
                                                             event_connection.current_data_size_);
            if (event_connection.read_now_ == -1) {
                TerminateEvent(socket);
                return;
            } else if (event_connection.read_now_ ==
                       event_connection.body_size_ - event_connection.current_data_size_) {
                event_connection.state_ = Connection::State::BLOCKON_RADD;
                // Continue in Connection::State::BLOCKON_RADD
            } else {
                // Not enough data in socket
                event_connection.current_data_size_ -= event_connection.read_now_;
                event_connection.state_ = Connection::State::BLOCKON_RDATA;
                return;
            }

        }

        if (event_connection.state_ == Connection::State::BLOCKON_RADD) {

            event_connection.state_ = Connection::State::BLOCKON_NONE;

            if (event_connection.current_buffer_size_ < event_connection.addition_len_) {

                event_connection.state_ = Connection::State::BLOCKON_RADD;

                event_connection.read_now_ = ReadStrict_NonBlock(socket,
                                                                 event_connection.buffer_ +
                                                                 event_connection.current_buffer_size_,
                                                                 event_connection.addition_len_ -
                                                                 event_connection.current_buffer_size_);
                if (event_connection.read_now_ == -1) {
                    TerminateEvent(socket);
                    return;
                } else if (event_connection.read_now_ ==
                           event_connection.addition_len_ - event_connection.current_buffer_size_) {
                    event_connection.state_ = Connection::State::BLOCKON_NONE;
                } else {
                    // Not enough data in socket
                    event_connection.current_buffer_size_ -= event_connection.read_now_;
                    event_connection.state_ = Connection::State::BLOCKON_RADD;
                    return;
                }
                event_connection.current_buffer_size_ = event_connection.addition_len_;
            }

            if (strncmp(event_connection.buffer_, event_connection.addition_, event_connection.addition_len_) != 0) {
                throw std::runtime_error("Incorrect command format");
            }
            event_connection.parsed_ = event_connection.addition_len_;
        }

        std::string server_ans;
        event_connection.command_->Execute(*(worker_->ps_),
                                           std::string(event_connection.data_block_, event_connection.body_size_),
                                           server_ans);
        event_connection.answers_.push(server_ans + "\r\n");
        WriteEvent(socket);
        event_connection.parser_.Reset();
    } catch (std::exception &exception){
        std::cout << "SERVER_ERROR " << exception.what() << std::endl;
        std::stringstream server_ans_stream;
        server_ans_stream << "SERVER_ERROR " << exception.what() << '\r' << '\n';
        std::string server_ans = server_ans_stream.str();

        event_connection.answers_.push(server_ans);
        WriteEvent(socket);

        event_connection.current_buffer_size_ = 0;
        event_connection.parsed_ = 0;
        event_connection.parser_.Reset();
    }
}

// See Worker.h
void Worker::EpollManager::WriteEvent(int socket) {
    std::cout << "network debug: " << __PRETTY_FUNCTION__ << std::endl;

    Connection& event_connection = connections_[socket];
    ssize_t wrote{0};

    while(!event_connection.answers_.empty()){
        std::string& cur_ans = event_connection.answers_.front();
        wrote = WriteStrict_NonBlock(socket, cur_ans.c_str(), cur_ans.length() - event_connection.first_ans_bias_);
        if(wrote == -1){
            // Error occured in send call
            TerminateEvent(socket);
        } else if(wrote == cur_ans.length() - event_connection.first_ans_bias_){
            // send wrote all data without blocking, continue sending answers
            event_connection.first_ans_bias_ = 0;
            event_connection.answers_.pop();
        } else {
            // send operation would block
            event_connection.first_ans_bias_ += wrote;
            return;
        }
    }
}


} // namespace NonBlocking
} // namespace Network
} // namespace Afina
