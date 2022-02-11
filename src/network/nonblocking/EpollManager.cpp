#include <iostream>

#include <unistd.h>
#include <sys/socket.h>
#include <cstring>
#include <afina/execute/Command.h>
#include <sys/stat.h>
#include <fcntl.h>

#include "EpollManager.h"
#include "Utils.h"
// TODO: see Reactor

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
    while (wrote < len) {
        // MSG_DONTWAIT is omitted due to nonblocking socket
        if ((wrote_now = this->WriteTo_NonBlock(source + wrote, len - wrote)) == -1) {
            if (errno == EAGAIN || errno == EWOULDBLOCK) {
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
            if (errno == EAGAIN || errno == EWOULDBLOCK) {
                return read;
            }
            std::cout << "resv error: " << std::string(strerror(errno)) << std::endl;
            return -1;
        } else if (read_now == 0) {
            return 0;
        } else {
            read += read_now;
        }
    }
    return read;
}


// See EpollManager.h
ServerSocketConnection::ServerSocketConnection(int server_socket) noexcept : Connection(server_socket) {}

// See EpollManager.h
int ServerSocketConnection::AcceptOn() noexcept {
    return accept(descriptor_, nullptr, nullptr);
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
FifoConnection::FifoConnection(const std::string& fifo_path, FifoConnection::FifoType type, int other_leg) noexcept :
        fifo_path_{fifo_path}, type_{type}, other_leg_{other_leg} {};

// See EpollManager.h
void FifoConnection::OpenConn() throw() {
    if(mkfifo(fifo_path_.c_str(), 0666) == -1){
        if(errno != EEXIST) {
            throw std::runtime_error("Fifo file mkfifo() failed. " + std::string(strerror(errno)));
        }
    }

    int flags = O_NONBLOCK;
    if(type_ == FifoConnection::FifoType::FIFO_READ){
        flags |= O_RDONLY;
    } else if (type_ == FifoConnection::FifoType::FIFO_WRITE){
        flags |= O_WRONLY;
    } else {
        flags |= O_RDWR;
    }
    if((descriptor_ = open(fifo_path_.c_str(), flags)) == -1){
        throw std::runtime_error("Fifo file open() failed. " + std::string(strerror(errno)));
    }

    make_socket_non_blocking(descriptor_);
}

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
EpollManager::EpollManager(int server_socket, const std::string& fifo_read_path, const std::string& fifo_write_path, Worker *worker) throw() : worker_{
        worker} {
    std::cout << "network debug: " << __PRETTY_FUNCTION__ << std::endl;

    connections_fd_.insert(server_socket);
    connections_[server_socket] = new ServerSocketConnection(server_socket);

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
        delete connections_[server_socket];
        delete events_;
        throw std::runtime_error("epoll_ctl: " + std::string(strerror(errno)));
    }

    try {
        AddFifoPair(fifo_read_path, fifo_write_path);
    } catch (...){
        delete connections_[server_socket];
        delete events_;
        throw;
    }

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
        if (errno == EINTR) {
            return;
        }
        throw std::runtime_error("epoll_wait: " + std::string(strerror(errno)));
    }
    for (int i = 0; i < events_num; ++i) {
        event_socket = events_[i].data.fd;
        Connection *connection = connections_[event_socket];

        if (typeid(*connection) == typeid(ServerSocketConnection)) {
            // Event on server socket
            auto serv_conn = dynamic_cast<ServerSocketConnection *>(connection);

            if (events_[i].events & EPOLLERR) {
                std::cout << "Error happened on server socket. Client sockets are monitored" << std::endl;
                TerminateEvent(serv_conn);
            } else if (events_[i].events & EPOLLHUP) {
                std::cout << "Server socket closed connection. Client sockets are monitored" << std::endl;
                TerminateEvent(serv_conn);
            } else if (events_[i].events & EPOLLIN) {
                AcceptEvent(serv_conn);
            } else {
                //TODO: is server socket valid after signaled unknown event
                std::cout << "Unknown event happened on server socket. Client sockets are monitored" << std::endl;
            }

        } else if (typeid(*connection) == typeid(SocketConnection)) {
            // Event on client socket

            if (events_[i].events & EPOLLERR) {
                std::cout << "Error happened on client socket" << std::endl;
                TerminateEvent(connection);
            } else if (events_[i].events & EPOLLHUP) {
                std::cout << "Client socket close connection" << std::endl;
                TerminateEvent(connection);
            } else {
                if (connections_fd_.count(event_socket) != 0 &&
                    events_[i].events & EPOLLIN) {
                    ReadEvent(connection);
                }
                if (connections_.count(event_socket) != 0 &&
                    events_[i].events & EPOLLOUT) {
                    WriteEvent(connection);
                }
                if (!(events_[i].events & EPOLLIN) && !(events_[i].events & EPOLLOUT)) {
                    //TODO: is client socket valid after signaled unknown event
                    std::cout << "Unknown event happened on client socket" << std::endl;
                }
            }

        } else if (typeid(*connections_[event_socket]) == typeid(FifoConnection)) {
            // Event in fifo file

            if (events_[i].events & EPOLLERR) {
                std::cout << "Error happened in fifo file" << std::endl;
                TerminateEvent(connection);
                continue;
            }
            // We ought to read all data placed in file even if
            // client closed its end of the connection (i.e. EPOLLHUP came)
            if (events_[i].events & EPOLLIN) {
                ReadEvent(connection);
            }

            // EPOLLHUB merely indicates that client closed its end of the connection
            // so we ought to close connection and than connect again
            if (events_[i].events & EPOLLHUP) {
                std::cout << "Client closed its end of the connection" << std::endl;

                // Reinitialate connection (continue monitoring fifo file)
                ReconnectEvent(connection);
                continue;
            }
            if (events_[i].events & EPOLLOUT) {
                WriteEvent(connection);
            }
            if (!(events_[i].events & EPOLLIN) && !(events_[i].events & EPOLLOUT) && !(events_[i].events & EPOLLHUP)) {
                //TODO: is client socket valid after signaled unknown event
                std::cout << "Unknown event happened on client socket" << std::endl;
            }

        }
    }
}

// See EpollManager.h
void EpollManager::AcceptEvent(ServerSocketConnection *connection) throw() {
    std::cout << "network debug: " << __PRETTY_FUNCTION__ << std::endl;

    int new_socket;
    if ((new_socket = connection->AcceptOn()) == -1) {
        TerminateEvent(connection);
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
void EpollManager::AddFifoPair(const std::string& fifo_read_path, const std::string& fifo_write_path) throw() {
    std::cout << "network debug: " << __PRETTY_FUNCTION__ << std::endl;

    // fifo_..._fd - already non blocking fifo file descriptor

    // EPOLLERR, EPOLLHUP - are always monitored
    epoll_event fifo_event;

    FifoConnection *read_fifo_conn = nullptr;
    FifoConnection *write_fifo_conn = nullptr;

    // Add input fifo file to monitoring if necessary
    if (fifo_read_path.length()) {
        read_fifo_conn = new FifoConnection(fifo_read_path, FifoConnection::FifoType::FIFO_READ, -1);
        try {
            read_fifo_conn->OpenConn();
        } catch (...){
            delete read_fifo_conn;
            throw;
        }

        connections_fd_.insert(read_fifo_conn ->descriptor_);
        connections_[read_fifo_conn->descriptor_] = read_fifo_conn ;


        fifo_event.events = EPOLLIN;
        fifo_event.data.fd = read_fifo_conn ->descriptor_;

        if (epoll_ctl(epoll_fd_, EPOLL_CTL_ADD, read_fifo_conn->descriptor_, &fifo_event) == -1) {
            delete read_fifo_conn;
            throw std::runtime_error("epoll_ctl: " + std::string(strerror(errno)));
        }
    }

    // Add output fifo file to monitoring if necessary
    if (fifo_write_path.length()) {
        write_fifo_conn = new FifoConnection(fifo_write_path, FifoConnection::FifoType::FIFO_WRITE, -1);
        try {
            write_fifo_conn->OpenConn();
        } catch (...){
            delete write_fifo_conn;
            throw;
        }

        connections_fd_.insert(write_fifo_conn->descriptor_);
        connections_[write_fifo_conn->descriptor_] = write_fifo_conn;


        fifo_event.events = EPOLLIN;
        fifo_event.data.fd = write_fifo_conn->descriptor_;

        if (epoll_ctl(epoll_fd_, EPOLL_CTL_ADD, write_fifo_conn->descriptor_, &fifo_event) == -1) {
            delete write_fifo_conn;
            throw std::runtime_error("epoll_ctl: " + std::string(strerror(errno)));
        }
    }

    if(read_fifo_conn != nullptr && write_fifo_conn != nullptr){
        read_fifo_conn->other_leg_ = write_fifo_conn->descriptor_;
        write_fifo_conn->other_leg_ = read_fifo_conn->descriptor_;
    }

}

// See EpollManager.h
void EpollManager::TerminateEvent(Connection *connection) throw() {

    // Part applied only for fifo files
    if(typeid(*connection) == typeid(FifoConnection)){
        auto fifo_conn = dynamic_cast<FifoConnection *>(connection);
        // If other leg exists yet "break" connection with closing leg
        if(fifo_conn->other_leg_ != -1){
            auto other_leg = dynamic_cast<FifoConnection *>(connections_[fifo_conn->other_leg_]);
            other_leg->other_leg_ = -1;
        }
    }

    // Part applied only for fifo files and client sockets
    if(typeid(*connection) == typeid(SocketConnection) ||
       typeid(*connection) == typeid(FifoConnection)) {
        connections_fd_.erase(connection->descriptor_);
        connections_.erase(connection->descriptor_);
    }

    // Warning! Set event to nullptr is error before kernel 2.6.9
    if (epoll_ctl(epoll_fd_, EPOLL_CTL_DEL, connection->descriptor_, nullptr) == -1) {
        std::cout << std::string(strerror(errno));
        throw std::runtime_error("epoll_ctl: ");// + std::string(strerror(errno)));
    }

    // Part applied only for fifo files and client sockets
    // Because server sockets are shared resource between different workers
    if(typeid(*connection) == typeid(SocketConnection) ||
       typeid(*connection) == typeid(FifoConnection)) {
        connection->CloseConn();
    }
    
    delete connection;
}

// See EpollManager.h
void EpollManager::ReconnectEvent(Connection *connection) throw() {

    // Part applied only for fifo files
    if(typeid(*connection) == typeid(FifoConnection)) {
        auto fifo_conn = dynamic_cast<FifoConnection *>(connection);

        FifoConnection *other_leg = nullptr;
        // If other leg exists yet "break" connection with closing leg
        if (fifo_conn->other_leg_ != -1) {
            other_leg = dynamic_cast<FifoConnection *>(connections_[fifo_conn->other_leg_]);
            other_leg->other_leg_ = -1;
        }

        connections_fd_.erase(fifo_conn->descriptor_);
        connections_.erase(fifo_conn->descriptor_);

        // Warning! Set event to nullptr is error before kernel 2.6.9
        if (epoll_ctl(epoll_fd_, EPOLL_CTL_DEL, fifo_conn->descriptor_, nullptr) == -1) {
            std::cout << std::string(strerror(errno));
            throw std::runtime_error("epoll_ctl: ");// + std::string(strerror(errno)));
        }

        fifo_conn->CloseConn();

        fifo_conn->OpenConn();

        connections_fd_.insert(fifo_conn ->descriptor_);
        connections_[fifo_conn->descriptor_] = fifo_conn;

        epoll_event fifo_event;
        if(fifo_conn->type_ == FifoConnection::FifoType::FIFO_READ){
            fifo_event.events = EPOLLIN;
        } else if(fifo_conn->type_ == FifoConnection::FifoType::FIFO_WRITE){
            fifo_event.events = EPOLLOUT;
        } else {
            fifo_event.events = EPOLLIN | EPOLLOUT;
        }
        fifo_event.data.fd = fifo_conn->descriptor_;

        if (epoll_ctl(epoll_fd_, EPOLL_CTL_ADD, fifo_conn->descriptor_, &fifo_event) == -1) {
            throw std::runtime_error("epoll_ctl: " + std::string(strerror(errno)));
        }

        if(other_leg != nullptr){
            other_leg->other_leg_ = fifo_conn->descriptor_;
        }
    }

}

// See EpollManager.h
void EpollManager::ReadEvent(Connection *connection) throw() {
    std::cout << "network debug: " << __PRETTY_FUNCTION__ << std::endl;

    if (connection->state_ != Connection::State::BLOCKON_RADD &&
        connection->state_ != Connection::State::BLOCKON_RDATA &&
        connection->state_ != Connection::State::BLOCKON_RCOM &&
        connection->state_ != Connection::State::BLOCKON_NONE) {
        throw std::runtime_error("Connection in inconsistent state");
    }
    // TODO: some commands may not be read
    if (!worker_->running_.load()) {
        TerminateEvent(connection);
        return;
    }

    try {
        if (connection->state_ == Connection::State::BLOCKON_NONE ||
            connection->state_ == Connection::State::BLOCKON_RCOM) {
            connection->parsed_now_ = 0;
            connection->read_now_ = 0;
            while (!connection->parser_.Parse(connection->buffer_ + connection->parsed_,
                                                   connection->current_buffer_size_ - connection->parsed_,
                                                   connection->parsed_now_)) {
                connection->parsed_ += connection->parsed_now_;

                if (connection->state_ == Connection::State::BLOCKON_NONE) {
                    if (connection->current_buffer_size_ == connection->max_buffer_size_) {
                        connection->parsed_ = 0;
                        connection->current_buffer_size_ = 0;
                    }
                }

                connection->read_now_ = connection->ReadFrom_NonBlock(
                        connection->buffer_ + connection->current_buffer_size_,
                        connection->max_buffer_size_ - connection->current_buffer_size_);

                if (connection->read_now_ == -1) {
                    if (errno == EAGAIN || errno == EWOULDBLOCK) {
                        connection->state_ = Connection::State::BLOCKON_RCOM;
                        return;
                    }
                    std::cout << "recv error: " << std::string(strerror(errno)) << std::endl;
                    TerminateEvent(connection);
                    return;
                } else if (connection->read_now_ == 0) {
                    // In all connection variants (either socket or fifo) this place
                    // is reached when client orderly closed connection.
                    // Somehow we may try to reestablish connection, but of socket case it quet hard
                    // so reestablishing is avalible only for fifo files
                    if(typeid(*connection) == typeid(FifoConnection)){
                        connection->state_ = Connection::State::BLOCKON_RCOM;
                        return;
                        // Reconnection are to be performed in EPOLLHUP handler
                    }
                    TerminateEvent(connection);
                    return;
                } else {
                    connection->current_buffer_size_ += connection->read_now_;
                }
            }
            connection->parsed_ += connection->parsed_now_;


            connection->command_ = std::move(
                    std::shared_ptr<Execute::Command>(connection->parser_.Build(connection->body_size_)));
            if (connection->body_size_ > connection->max_data_size_) {
                throw std::runtime_error("Too long data_block");
            } else if (connection->body_size_ == 0) {
                connection->current_buffer_size_ =
                        connection->current_buffer_size_ - connection->parsed_;
                memmove(connection->buffer_,
                        connection->buffer_ + connection->parsed_,
                        connection->current_buffer_size_);
                connection->parsed_ = 0;
                connection->state_ = Connection::State::BLOCKON_NONE;
            } else {
                connection->current_data_size_ = std::min(
                        connection->current_buffer_size_ - connection->parsed_,
                        static_cast<size_t >(connection->body_size_));

                connection->current_buffer_size_ =
                        connection->current_buffer_size_ - connection->parsed_ -
                        connection->current_data_size_;

                std::memcpy(connection->data_block_,
                            connection->buffer_ + connection->parsed_,
                            connection->current_data_size_);

                std::memmove(connection->buffer_,
                             connection->buffer_ + connection->parsed_ + connection->current_data_size_,
                             connection->current_buffer_size_);


                connection->state_ = Connection::State::BLOCKON_RDATA;
                // Continue in Connection::State::BLOCKON_RDATA
            }
        }
        if (connection->state_ == Connection::State::BLOCKON_RDATA) {

            connection->read_now_ = connection->ReadFromStrict_NonBlock(
                    connection->data_block_ + connection->current_data_size_,
                    connection->body_size_ - connection->current_data_size_);

            if (connection->read_now_ == -1) {
                TerminateEvent(connection);
                return;
            } else if (connection->read_now_ ==
                       connection->body_size_ - connection->current_data_size_) {
                connection->state_ = Connection::State::BLOCKON_RADD;
                // Continue in Connection::State::BLOCKON_RADD
            } else if(connection->read_now_ == 0) {
                // In all connection variants (either socket or fifo) this place
                // is reached when client orderly closed connection.
                // Somehow we may try to reestablish connection, but of socket case it quet hard
                // so reestablishing is avalible only for fifo files
                if(typeid(*connection) == typeid(FifoConnection)){
                    connection->state_ = Connection::State::BLOCKON_RDATA;
                    return;
                    // Reconnection are to be performed in EPOLLHUP handler
                }
                TerminateEvent(connection);
                return;
            } else {
                // Not enough data in socket
                connection->current_data_size_ -= connection->read_now_;
                connection->state_ = Connection::State::BLOCKON_RDATA;
                return;
            }

        }

        if (connection->state_ == Connection::State::BLOCKON_RADD) {

            connection->state_ = Connection::State::BLOCKON_NONE;

            if (connection->current_buffer_size_ < connection->addition_len_) {

                connection->state_ = Connection::State::BLOCKON_RADD;

                connection->read_now_ = connection->ReadFromStrict_NonBlock(
                        connection->buffer_ + connection->current_buffer_size_,
                        connection->addition_len_ - connection->current_buffer_size_);

                if (connection->read_now_ == -1) {
                    TerminateEvent(connection);
                    return;
                } else if (connection->read_now_ ==
                           connection->addition_len_ - connection->current_buffer_size_) {
                    connection->state_ = Connection::State::BLOCKON_NONE;
                } else if(connection->read_now_ == 0){
                    // In all connection variants (either socket or fifo) this place
                    // is reached when client orderly closed connection.
                    // Somehow we may try to reestablish connection, but of socket case it quet hard
                    // so reestablishing is avalible only for fifo files
                    if(typeid(*connection) == typeid(FifoConnection)){
                        connection->state_ = Connection::State::BLOCKON_RADD;
                        return;
                        // Reconnection are to be performed in EPOLLHUP handler
                    }
                    TerminateEvent(connection);
                    return;
                } else {
                    // Not enough data in socket
                    connection->current_buffer_size_ -= connection->read_now_;
                    connection->state_ = Connection::State::BLOCKON_RADD;
                    return;
                }
                connection->current_buffer_size_ = connection->addition_len_;
            }

            if (strncmp(connection->buffer_, connection->addition_, connection->addition_len_) != 0) {
                throw std::runtime_error("Incorrect command format");
            }
            connection->parsed_ = connection->addition_len_;
        }

        std::string server_ans;
        connection->command_->Execute(*(worker_->ps_),
                                           std::string(connection->data_block_, connection->body_size_),
                                           server_ans);
        if(typeid(*connection) == typeid(FifoConnection)){
            auto fifo_conn = dynamic_cast<FifoConnection *>(connection);
            if(fifo_conn->other_leg_ != -1){
                // Important that other_leg_ field always in consistent state
                auto other_leg = dynamic_cast<FifoConnection *>(connections_[fifo_conn->other_leg_]);
                other_leg->answers_.push(server_ans + "\r\n");
                WriteEvent(other_leg);
            }
        } else {
            connection->answers_.push(server_ans + "\r\n");
            WriteEvent(connection);
        }
        connection->parser_.Reset();
        if(connection->current_buffer_size_ - connection->parsed_ > 0){
            ReadEvent(connection);
        }
    } catch (std::exception &exception) {
        std::cout << "SERVER_ERROR " << exception.what() << std::endl;
        std::stringstream server_ans_stream;
        server_ans_stream << "SERVER_ERROR " << exception.what() << '\r' << '\n';
        std::string server_ans = server_ans_stream.str();

        if(typeid(*connection) == typeid(FifoConnection)){
            auto fifo_conn = dynamic_cast<FifoConnection *>(connection);
            if(fifo_conn->other_leg_ != -1){
                // Important that other_leg_ field always in consistent state
                auto other_leg = dynamic_cast<FifoConnection *>(connections_[fifo_conn->other_leg_]);
                other_leg->answers_.push(server_ans);
                WriteEvent(other_leg);
            }
        } else {
            connection->answers_.push(server_ans);
            WriteEvent(connection);
        }

        connection->current_buffer_size_ = 0;
        connection->parsed_ = 0;
        connection->parser_.Reset();
    }
}

// See EpollManager.h
void EpollManager::WriteEvent(Connection *connection) noexcept {
    std::cout << "network debug: " << __PRETTY_FUNCTION__ << std::endl;

    ssize_t wrote{0};

    // TODO: Implement iovec
    // TODO: Write only by epoll events
    while (!connection->answers_.empty()) {
        std::string &cur_ans = connection->answers_.front();
        // TODO: move pointer
        wrote = connection->WriteToStrict_NonBlock(cur_ans.c_str(), cur_ans.length() - connection->first_ans_bias_);
        if (wrote == -1) {
            // Error occured in send call
            TerminateEvent(connection);
        } else if (wrote == cur_ans.length() - connection->first_ans_bias_) {
            // send wrote all data without blocking, continue sending answers
            connection->first_ans_bias_ = 0;
            connection->answers_.pop();
        } else {
            // send operation would block
            connection->first_ans_bias_ += wrote;
            return;
        }
    }
}


} // namespace NonBlocking
} // namespace Network
} // namespace Afina