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

    explicit Connection(int descriptor) noexcept ;

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

    FifoConnection(int fifo_fd, FifoConnection::FifoType type, int other_leg) noexcept ;

    ssize_t WriteTo_NonBlock(const char *source, size_t len) noexcept override ;

    ssize_t ReadFrom_NonBlock(char *dest, size_t len) noexcept override ;

    void CloseConn() noexcept override ;

private:
    friend EpollManager;

    // Store read leg for write fifo file and
    // write fifo leg for read fifo file
    int other_leg_;

};


class EpollManager {
public:

    explicit EpollManager(int server_socket, int fifo_read_fd, int fifo_write_fd, Worker *worker) throw() ;

    ~EpollManager() noexcept ;

    void Stop() noexcept ;

    void WaitEvent() throw() ;

    void AcceptEvent() throw() ;

    void AddFifoPair(int fifo_read_fd, int fifo_write_fd) throw() ;

    void TerminateEvent(int descriptor) throw() ;

    void TerminateServerEvent(int server_sock) throw() ;

    void ReadEvent(int socket) throw() ;

    void WriteEvent(int socket) noexcept ;

private:

    static constexpr int max_events_{100};
//    // -1 means infinity time of event waiting
    static constexpr int max_timeout_{10000};

    // Due no support in Ubuntu 16.04 (old epoll.h, but kernel support this flag)
    enum EPOLLFLAG {
        EPOLLEXCLUSIVE = (1u << 28)
    };

    Worker *worker_;

    std::unordered_set<int> connections_fd_;
    std::unordered_map<int, Connection *> connections_;

    struct epoll_event * events_;
    int epoll_fd_;

    //TODO: Make implementation without this field
    int server_socket_;
};


} // namespace NonBlocking
} // namespace Network
} // namespace Afina

#endif //AFINA_EPOLLMANAGER_H
