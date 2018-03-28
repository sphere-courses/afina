#ifndef AFINA_EPOLLMANAGER_H
#define AFINA_EPOLLMANAGER_H


#include "Worker.h"

namespace Afina {
namespace Network {
namespace NonBlocking {

// Forward declaration, see Worker.h
class Worker;

// Forward declaration, see EpollManager.h
class EpollManager;


class Connection {
public:

    explicit Connection(int descriptor = -1) noexcept ;

    virtual ssize_t WriteToStrict_NonBlock(const char *source, size_t len) noexcept ;

    virtual ssize_t ReadFromStrict_NonBlock(char *dest, size_t len) noexcept ;

    virtual ssize_t WriteTo_NonBlock(const char *source, size_t len) noexcept = 0;

    virtual ssize_t ReadFrom_NonBlock(char *dest, size_t len) noexcept = 0;

    virtual void CloseConn() noexcept = 0;

    int descriptor_;

private:
    friend EpollManager;

    enum State {
        BLOCKON_NONE,
        BLOCKON_RCOM,
        BLOCKON_RDATA,
        BLOCKON_RADD
    };

    State state_{BLOCKON_NONE};
    std::queue<std::string> answers_;
    size_t first_ans_bias_{0};

    // Fields describing current connection-parsing state

    static constexpr int max_buffer_size_{1024}, max_data_size_{1024};

    static char addition_[];
    static size_t addition_len_;

    char buffer_[max_buffer_size_];
    char data_block_[max_data_size_];

    size_t current_buffer_size_{0}, parsed_{0};

    Protocol::Parser parser_;

    size_t parsed_now_{0};
    ssize_t read_now_{0};

    uint32_t body_size_{0};

    size_t current_data_size_{0};

    std::shared_ptr<Execute::Command> command_;
};

class ServerSocketConnection : public Connection {
public:

    explicit ServerSocketConnection(int server_socket) noexcept ;

    int AcceptOn() noexcept ;

private:

    ssize_t WriteTo_NonBlock(const char *source, size_t len) noexcept override {};

    ssize_t ReadFrom_NonBlock(char *dest, size_t len) noexcept override {};

    void CloseConn() noexcept override {};

};

class SocketConnection : public Connection {

public:

    explicit SocketConnection(int socket) noexcept ;

    ssize_t WriteTo_NonBlock(const char *source, size_t len) noexcept override ;

    ssize_t ReadFrom_NonBlock(char *dest, size_t len) noexcept override ;

    void CloseConn() noexcept override ;


private:
    friend EpollManager;

};

class FifoConnection : public Connection {

public:
    enum class FifoType {
        FIFO_READ,
        FIFO_WRITE,
        FIFO_RW
    };

    FifoType type_;

    FifoConnection(const std::string& fifo_path, FifoConnection::FifoType type, int other_leg = -1) noexcept ;

    void OpenConn() throw() ;

    ssize_t WriteTo_NonBlock(const char *source, size_t len) noexcept override ;

    ssize_t ReadFrom_NonBlock(char *dest, size_t len) noexcept override ;

    void CloseConn() noexcept override ;

private:
    friend EpollManager;

    // Store read leg for write fifo file and
    // write fifo leg for read fifo file
    int other_leg_;

    std::string fifo_path_;

};


class EpollManager {
public:

    explicit EpollManager(int server_socket, const std::string& fifo_read_path, const std::string& fifo_write_path, Worker *worker) throw() ;

    ~EpollManager() noexcept ;

    void Stop() noexcept ;

    void WaitEvent() throw() ;

    void AcceptEvent(ServerSocketConnection *connection) throw() ;

    void AddFifoPair(const std::string& fifo_read_path, const std::string& fifo_write_path) throw() ;

    void TerminateEvent(Connection *connection) throw() ;

    void ReconnectEvent(Connection *connection) throw();

    void ReadEvent(Connection *connection) throw() ;

    void WriteEvent(Connection *connection) noexcept ;

private:

    static constexpr int max_events_{100};
//    // -1 means infinity time of event waiting
    static constexpr int max_timeout_{-1};

    // Due no support in Ubuntu 16.04 (old epoll.h, but kernel support this flag)
    enum EPOLLFLAG {
        EPOLLEXCLUSIVE = (1u << 28)
    };

    Worker *worker_;

    std::unordered_set<int> connections_fd_;
    std::unordered_map<int, Connection *> connections_;

    struct epoll_event * events_;
    int epoll_fd_;

};


} // namespace NonBlocking
} // namespace Network
} // namespace Afina

#endif //AFINA_EPOLLMANAGER_H
