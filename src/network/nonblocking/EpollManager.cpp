#include <iostream>

#include <unistd.h>
#include <sys/socket.h>
#include <cstring>
#include <afina/execute/Command.h>

#include "EpollManager.h"
#include "Utils.h"


namespace Afina {
namespace Network {
namespace NonBlocking {

// Define static variables
char Connection::addition_[] = "\r\n";
size_t Connection::addition_len_ = sizeof(addition_) - 1;

constexpr int EpollManager::max_events_;
constexpr int EpollManager::max_timeout_;

// See EpollManager.h
Connection::Connection(int descriptor) noexcept : descriptor_{descriptor} {}

// See EpollManager.h
ssize_t Connection::WriteToStrict_NonBlock(const char *source, size_t len) noexcept {
    ssize_t wrote_now{0};
    size_t wrote{0};
    while(wrote < len){
        // MSG_DONTWAIT is omitted due to nonblocking socket
        if((wrote_now = this->WriteTo_NonBlock(source + wrote, len - wrote)) == -1){
            if(errno == EAGAIN || errno == EWOULDBLOCK){
                return wrote;
            }
            std::cout << "send error: " << std::string(strerror(errno)) << std::endl;
            return -1;
        } else {
            wrote += wrote_now;
        }
    }
    return wrote;
}

// See EpollManager.h
ssize_t Connection::ReadFromStrict_NonBlock(char *dest, size_t len) noexcept {
    ssize_t read = 0;
    ssize_t read_now = 0;
    while (read < len) {
        // MSG_DONTWAIT is omitted due to nonblocking socket
        read_now = this->ReadFrom_NonBlock(dest + read, len - read);
        if (read_now == -1) {
            if(errno == EAGAIN || errno == EWOULDBLOCK){
                return read;
            }
            std::cout << "resv error: " << std::string(strerror(errno)) << std::endl;
            return -1;
        } else if(read_now == 0){
            return -1;
        } else {
            read += read_now;
        }
    }
    return read;
}


// See EpollManager.h
SocketConnection::SocketConnection(int socket) noexcept : Connection{socket} {}

// See EpollManager.h
ssize_t SocketConnection::WriteTo_NonBlock(const char *source, size_t len) noexcept {
    return send(descriptor_, source, len, 0);
}

// See EpollManager.h
ssize_t SocketConnection::ReadFrom_NonBlock(char *dest, size_t len) noexcept {
    return recv(descriptor_, dest, len, 0);
}

// See EpollManager.h
void SocketConnection::CloseConn() noexcept {
    shutdown(descriptor_, SHUT_RDWR);
    close(descriptor_);
}


// See EpollManager.h
FifoConnection::FifoConnection(int fifo_fd, FifoConnection::FifoType type, int other_leg) noexcept :
        Connection(fifo_fd), type_{type}, other_leg_{other_leg} {};

// See EpollManager.h
ssize_t FifoConnection::WriteTo_NonBlock(const char *source, size_t len) noexcept {
    return write(descriptor_, source, len);
}

// See EpollManager.h
ssize_t FifoConnection::ReadFrom_NonBlock(char *dest, size_t len) noexcept {
    return read(descriptor_, dest, len);
}

// See EpollManager.h
void FifoConnection::CloseConn() noexcept {
    close(descriptor_);
}


// See EpollManager.h
EpollManager::EpollManager(int server_socket, int fifo_read_fd, int fifo_write_fd, Worker *worker) throw() :
        server_socket_(server_socket), worker_{worker}{
    std::cout << "network debug: " << __PRETTY_FUNCTION__ << std::endl;

    // Since UNIX kernel 2.6.7 it is okay to pass any positive size_t argument
    // to epoll_create with no difference
    if ((epoll_fd_ = epoll_create(1)) == -1) {
        throw std::runtime_error("epoll_create: " + std::string(strerror(errno)));
    }

    events_ = new epoll_event[100];

    // EPOLLERR, EPOLLHUP - are always monitored
    epoll_event server_event;
    server_event.events = EPOLLFLAG::EPOLLEXCLUSIVE | EPOLLIN;
    server_event.data.fd = server_socket;

    if (epoll_ctl(epoll_fd_, EPOLL_CTL_ADD, server_socket, &server_event) == -1) {
        throw std::runtime_error("epoll_ctl: " + std::string(strerror(errno)));
    }

    AddFifoPair(fifo_read_fd, fifo_write_fd);
}


// See EpollManager.h
EpollManager::~EpollManager() noexcept {
    std::cout << "network debug: " << __PRETTY_FUNCTION__ << std::endl;

    for (auto connection : connections_) {
        connection.second->CloseConn();
        delete connection.second;
    }

    delete[] events_;
}

// See EpollManager.h
void EpollManager::Stop() noexcept {
    std::cout << "network debug: " << __PRETTY_FUNCTION__ << std::endl;

    close(epoll_fd_);
}

// See EpollManager.h
void EpollManager::WaitEvent() throw() {
    std::cout << "network debug: " << __PRETTY_FUNCTION__ << std::endl;

    int events_num;
    int event_socket;

    if ((events_num = epoll_pwait(epoll_fd_, events_, max_events_, max_timeout_, nullptr)) == -1) {
        if(errno == EINTR){
            return;
        }
        throw std::runtime_error("epoll_wait: " + std::string(strerror(errno)));
    }
    for (int i = 0; i < events_num; ++i) {
        event_socket = events_[i].data.fd;

        if (event_socket == server_socket_) {

            // Event on server socket
            if (events_[i].events & EPOLLERR) {
                std::cout << "Error happened on server socket. Client sockets are monitored" << std::endl;
                TerminateServerEvent(server_socket_);
            } else if (events_[i].events & EPOLLHUP) {
                std::cout << "Server socket closed connection. Client sockets are monitored" << std::endl;
                TerminateServerEvent(server_socket_);
            } else if (events_[i].events & EPOLLIN) {
                AcceptEvent();
            } else {
                //TODO: is server socket valid after signaled unknown event
                std::cout << "Unknown event happened on server socket. Client sockets are monitored" << std::endl;
            }

        } else {

            // Event on connection
            if(typeid(*connections_[event_socket]) == typeid(FifoConnection)){

                if (events_[i].events & EPOLLERR) {
                    std::cout << "Error happened on client socket" << std::endl;
                    TerminateEvent(event_socket);
                    continue;
                }
                if (events_[i].events & EPOLLHUP) {
                    std::cout << "Client socket close connection" << std::endl;
                    TerminateEvent(event_socket);
                    AddFifoPair(event_socket, -1);
                    // TODO: Reinitialate connection
                    // !!! INCORRECT !!!
                    // Do nothing because corresponding file descriptor is fifo file and
                    // EPOLLHUB merely indicates that client closed its end of the connection
                }
                // We ought to read all data placed in file even if client closed its end of the connection
                if (connections_fd_.count(event_socket) != 0 &&
                    events_[i].events & EPOLLIN) {
                    ReadEvent(event_socket);
                }
                if (connections_.count(event_socket) != 0 &&
                    events_[i].events & EPOLLOUT) {
                    WriteEvent(event_socket);
                }
                if (!(events_[i].events & EPOLLIN) && !(events_[i].events & EPOLLOUT) && !(events_[i].events & EPOLLHUP)) {
                    //TODO: is client socket valid after signaled unknown event
                    std::cout << "Unknown event happened on client socket" << std::endl;
                }

            } else {

                if (events_[i].events & EPOLLERR) {
                    std::cout << "Error happened on client socket" << std::endl;
                    TerminateEvent(event_socket);
                } else if (events_[i].events & EPOLLHUP) {
                    std::cout << "Client socket close connection" << std::endl;
                    TerminateEvent(event_socket);
                } else {
                    if (connections_fd_.count(event_socket) != 0 &&
                        events_[i].events & EPOLLIN) {
                        ReadEvent(event_socket);
                    }
                    if (connections_.count(event_socket) != 0 &&
                        events_[i].events & EPOLLOUT) {
                        WriteEvent(event_socket);
                    }
                    if (!(events_[i].events & EPOLLIN) && !(events_[i].events & EPOLLOUT)) {
                        //TODO: is client socket valid after signaled unknown event
                        std::cout << "Unknown event happened on client socket" << std::endl;
                    }
                }

            }
        }
    }
}

// See EpollManager.h
void EpollManager::AcceptEvent() throw() {
    std::cout << "network debug: " << __PRETTY_FUNCTION__ << std::endl;

    int new_socket;
    if ((new_socket = accept(server_socket_, nullptr, nullptr)) == -1) {
        TerminateServerEvent(server_socket_);
        //TODO: can we continue EpollManager?
        throw std::runtime_error("server_socket: " + std::string(strerror(errno)));
    }

    make_socket_non_blocking(new_socket);

    connections_fd_.insert(new_socket);
    connections_[new_socket] = new SocketConnection(new_socket);

    epoll_event client_event;
    client_event.events = EPOLLIN | EPOLLOUT;
    client_event.data.fd = new_socket;

    if (epoll_ctl(epoll_fd_, EPOLL_CTL_ADD, new_socket, &client_event) == -1) {
        throw std::runtime_error("epoll_ctl: " + std::string(strerror(errno)));
    }
}

// See EpollManager.h
void EpollManager::AddFifoPair(int fifo_read_fd, int fifo_write_fd) throw() {
    std::cout << "network debug: " << __PRETTY_FUNCTION__ << std::endl;

    // fifo_..._fd - already non blocking fifo file descriptor

    // EPOLLERR, EPOLLHUP - are always monitored
    epoll_event fifo_event;

    // Add input fifo file to monitoring if necessary
    if (fifo_read_fd != -1) {
        connections_fd_.insert(fifo_read_fd);
        connections_[fifo_read_fd] = new FifoConnection(fifo_read_fd, FifoConnection::FifoType::FIFO_READ, fifo_write_fd);

        fifo_event.events = EPOLLIN;
        fifo_event.data.fd = fifo_read_fd;

        if (epoll_ctl(epoll_fd_, EPOLL_CTL_ADD, fifo_read_fd, &fifo_event) == -1) {
            throw std::runtime_error("epoll_ctl: " + std::string(strerror(errno)));
        }
    }

    // Add output fifo file to monitoring if necessary
    if (fifo_write_fd != -1) {
        connections_fd_.insert(fifo_write_fd);
        connections_[fifo_write_fd] = new FifoConnection(fifo_write_fd, FifoConnection::FifoType::FIFO_WRITE, fifo_read_fd);

        fifo_event.events = EPOLLOUT;
        fifo_event.data.fd = fifo_write_fd;

        if (epoll_ctl(epoll_fd_, EPOLL_CTL_ADD, fifo_write_fd, &fifo_event) == -1) {
            throw std::runtime_error("epoll_ctl: " + std::string(strerror(errno)));
        }
    }
}

// See EpollManager.h
void EpollManager::TerminateEvent(int descriptor) throw() {
    std::cout << "network debug: " << __PRETTY_FUNCTION__ << std::endl;

    Connection *connection = connections_[descriptor];

    auto fifo_conn = dynamic_cast<FifoConnection *>(connection);
    if(fifo_conn != nullptr){
        // If other leg exists yet "break" connection with closing leg
        if(fifo_conn->other_leg_ != -1){
            auto other_leg = dynamic_cast<FifoConnection *>(connections_[fifo_conn->other_leg_]);
            other_leg->other_leg_ = -1;
        }
    }

    connections_fd_.erase(descriptor);
    connections_.erase(descriptor);

    // Warning! Set event to nullptr is error before kernel 2.6.9
    if (epoll_ctl(epoll_fd_, EPOLL_CTL_DEL, descriptor, nullptr) == -1) {
        std::cout << std::string(strerror(errno));
        throw std::runtime_error("epoll_ctl: ");// + std::string(strerror(errno)));
    }

    connection->CloseConn();
    delete connection;
}

// See EpollManager.h
void EpollManager::TerminateServerEvent(int server_sock) throw() {
    std::cout << "network debug: " << __PRETTY_FUNCTION__ << std::endl;

    // Warning! Set event to nullptr is error before kernel 2.6.9
    if(epoll_ctl(epoll_fd_, EPOLL_CTL_DEL, server_sock, nullptr) == -1){
        throw std::runtime_error("epoll_ctl: " + std::string(strerror(errno)));
    }
}

// See EpollManager.h
void EpollManager::ReadEvent(int descriptor) throw() {
    std::cout << "network debug: " << __PRETTY_FUNCTION__ << std::endl;

    Connection *event_connection = connections_[descriptor];
    if (event_connection->state_ != Connection::State::BLOCKON_RADD &&
        event_connection->state_ != Connection::State::BLOCKON_RDATA &&
        event_connection->state_ != Connection::State::BLOCKON_RCOM &&
        event_connection->state_ != Connection::State::BLOCKON_NONE) {
        throw std::runtime_error("Connection in inconsistent state");
    }

    if (!worker_->running_.load()) {
        TerminateEvent(descriptor);
        return;
    }

    try {
        if (event_connection->state_ == Connection::State::BLOCKON_NONE ||
            event_connection->state_ == Connection::State::BLOCKON_RCOM) {
            event_connection->parsed_now_ = 0;
            event_connection->read_now_ = 0;
            while (!event_connection->parser_.Parse(event_connection->buffer_ + event_connection->parsed_,
                                                   event_connection->current_buffer_size_ - event_connection->parsed_,
                                                   event_connection->parsed_now_)) {
                event_connection->parsed_ += event_connection->parsed_now_;

                if (event_connection->state_ == Connection::State::BLOCKON_NONE) {
                    if (event_connection->current_buffer_size_ == event_connection->max_buffer_size_) {
                        event_connection->parsed_ = 0;
                        event_connection->current_buffer_size_ = 0;
                    }
                }

                event_connection->read_now_ = event_connection->ReadFrom_NonBlock(
                        event_connection->buffer_ + event_connection->current_buffer_size_,
                        event_connection->max_buffer_size_ - event_connection->current_buffer_size_);

                if (event_connection->read_now_ == -1) {
                    if (errno == EAGAIN || errno == EWOULDBLOCK) {
                        event_connection->state_ = Connection::State::BLOCKON_RCOM;
                        return;
                    }
                    std::cout << "recv error: " << std::string(strerror(errno)) << std::endl;
                    TerminateEvent(descriptor);
                    return;
                } else if (event_connection->read_now_ == 0) {
                    TerminateEvent(descriptor);
                    return;
                } else {
                    event_connection->current_buffer_size_ += event_connection->read_now_;
                }
            }
            event_connection->parsed_ += event_connection->parsed_now_;


            event_connection->command_ = std::move(
                    std::shared_ptr<Execute::Command>(event_connection->parser_.Build(event_connection->body_size_)));
            if (event_connection->body_size_ > event_connection->max_data_size_) {
                throw std::runtime_error("Too long data_block");
            } else if (event_connection->body_size_ == 0) {
                event_connection->current_buffer_size_ =
                        event_connection->current_buffer_size_ - event_connection->parsed_;
                memmove(event_connection->buffer_,
                        event_connection->buffer_ + event_connection->parsed_,
                        event_connection->current_buffer_size_);
                event_connection->parsed_ = 0;
                event_connection->state_ = Connection::State::BLOCKON_NONE;
            } else {
                event_connection->current_data_size_ = std::min(
                        event_connection->current_buffer_size_ - event_connection->parsed_,
                        static_cast<size_t >(event_connection->body_size_));

                event_connection->current_buffer_size_ =
                        event_connection->current_buffer_size_ - event_connection->parsed_ -
                        event_connection->current_data_size_;

                std::memcpy(event_connection->data_block_,
                            event_connection->buffer_ + event_connection->parsed_,
                            event_connection->current_data_size_);

                std::memmove(event_connection->buffer_,
                             event_connection->buffer_ + event_connection->parsed_ + event_connection->current_data_size_,
                             event_connection->current_buffer_size_);


                event_connection->state_ = Connection::State::BLOCKON_RDATA;
                // Continue in Connection::State::BLOCKON_RDATA
            }
        }
        if (event_connection->state_ == Connection::State::BLOCKON_RDATA) {

            event_connection->read_now_ = event_connection->ReadFromStrict_NonBlock(
                    event_connection->data_block_ + event_connection->current_data_size_,
                    event_connection->body_size_ - event_connection->current_data_size_);

            if (event_connection->read_now_ == -1) {
                TerminateEvent(descriptor);
                return;
            } else if (event_connection->read_now_ ==
                       event_connection->body_size_ - event_connection->current_data_size_) {
                event_connection->state_ = Connection::State::BLOCKON_RADD;
                // Continue in Connection::State::BLOCKON_RADD
            } else {
                // Not enough data in socket
                event_connection->current_data_size_ -= event_connection->read_now_;
                event_connection->state_ = Connection::State::BLOCKON_RDATA;
                return;
            }

        }

        if (event_connection->state_ == Connection::State::BLOCKON_RADD) {

            event_connection->state_ = Connection::State::BLOCKON_NONE;

            if (event_connection->current_buffer_size_ < event_connection->addition_len_) {

                event_connection->state_ = Connection::State::BLOCKON_RADD;

                event_connection->read_now_ = event_connection->ReadFromStrict_NonBlock(
                        event_connection->buffer_ + event_connection->current_buffer_size_,
                        event_connection->addition_len_ - event_connection->current_buffer_size_);

                if (event_connection->read_now_ == -1) {
                    TerminateEvent(descriptor);
                    return;
                } else if (event_connection->read_now_ ==
                           event_connection->addition_len_ - event_connection->current_buffer_size_) {
                    event_connection->state_ = Connection::State::BLOCKON_NONE;
                } else {
                    // Not enough data in socket
                    event_connection->current_buffer_size_ -= event_connection->read_now_;
                    event_connection->state_ = Connection::State::BLOCKON_RADD;
                    return;
                }
                event_connection->current_buffer_size_ = event_connection->addition_len_;
            }

            if (strncmp(event_connection->buffer_, event_connection->addition_, event_connection->addition_len_) != 0) {
                throw std::runtime_error("Incorrect command format");
            }
            event_connection->parsed_ = event_connection->addition_len_;
        }

        std::string server_ans;
        event_connection->command_->Execute(*(worker_->ps_),
                                           std::string(event_connection->data_block_, event_connection->body_size_),
                                           server_ans);
        if(typeid(*event_connection) == typeid(FifoConnection)){
            auto fifo_conn = dynamic_cast<FifoConnection *>(event_connection);
            if(fifo_conn->other_leg_ != -1){
                // Important that other_leg_ field always in consistent state
                auto other_leg = dynamic_cast<FifoConnection *>(connections_[fifo_conn->other_leg_]);
                other_leg->answers_.push(server_ans + "\r\n");
                WriteEvent(fifo_conn->other_leg_);
            }
        } else {
            event_connection->answers_.push(server_ans + "\r\n");
            WriteEvent(descriptor);
        }
        event_connection->parser_.Reset();
    } catch (std::exception &exception) {
        std::cout << "SERVER_ERROR " << exception.what() << std::endl;
        std::stringstream server_ans_stream;
        server_ans_stream << "SERVER_ERROR " << exception.what() << '\r' << '\n';
        std::string server_ans = server_ans_stream.str();

        if(typeid(*event_connection) == typeid(FifoConnection)){
            auto fifo_conn = dynamic_cast<FifoConnection *>(event_connection);
            if(fifo_conn->other_leg_ != -1){
                // Important that other_leg_ field always in consistent state
                auto other_leg = dynamic_cast<FifoConnection *>(connections_[fifo_conn->other_leg_]);
                other_leg->answers_.push(server_ans);
                WriteEvent(fifo_conn->other_leg_);
            }
        } else {
            event_connection->answers_.push(server_ans);
            WriteEvent(descriptor);
        }

        event_connection->current_buffer_size_ = 0;
        event_connection->parsed_ = 0;
        event_connection->parser_.Reset();
    }
}

// See EpollManager.h
void EpollManager::WriteEvent(int descriptor) noexcept {
    std::cout << "network debug: " << __PRETTY_FUNCTION__ << std::endl;

    Connection *event_connection = connections_[descriptor];
    ssize_t wrote{0};

    while (!event_connection->answers_.empty()) {
        std::string &cur_ans = event_connection->answers_.front();
        wrote = event_connection->WriteToStrict_NonBlock(cur_ans.c_str(), cur_ans.length() - event_connection->first_ans_bias_);
        if (wrote == -1) {
            // Error occured in send call
            TerminateEvent(descriptor);
        } else if (wrote == cur_ans.length() - event_connection->first_ans_bias_) {
            // send wrote all data without blocking, continue sending answers
            event_connection->first_ans_bias_ = 0;
            event_connection->answers_.pop();
        } else {
            // send operation would block
            event_connection->first_ans_bias_ += wrote;
            return;
        }
    }
}


} // namespace NonBlocking
} // namespace Network
} // namespace Afina