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

namespace Afina {

// Forward declaration, see afina/Storage.h
class Storage;

namespace Network {
namespace NonBlocking {

/**
 * # Thread running epoll
 * On Start spaws background thread that is doing epoll on the given server
 * socket and process incoming connections and its data
 */
class Worker {
public:
    explicit Worker(std::shared_ptr<Afina::Storage> ps);

    ~Worker() = default;

    /**
     * Spaws new background thread that is doing epoll on the given server
     * socket. Once connection accepted it must be registered and being processed
     * on this thread
     */
    void Start(int server_socket);

    /**
     * Signal background thread to stop. After that signal thread must stop to
     * accept new connections and must stop read new commands from existing. Once
     * all readed commands are executed and results are send back to client, thread
     * must stop
     */
    void Stop();

    /**
     * Blocks calling thread until background one for this worker is actually
     * been destoryed
     */
    void Join();

protected:
    /**
     * Method executing by background thread
     */
    void OnRun(int server_socket);

private:
    static void * RunOnRunProxy(void *proxy_args);


    // Nested class to pass parameters of new connection through proxy function
    class ProxyArgs{
    public:
        Worker *worker_;
        int socket_;
    };


    class EpollManager{
    public:
        explicit EpollManager(int server_socket, Worker *worker);

        ~EpollManager();

        void Stop();

        void WaitEvent();

        void AcceptEvent();

        void TerminateEvent(int socket, bool is_server = false);

        void ReadEvent(int socket);

        void WriteEvent(int socket);

    private:
        class Connection{
        public:
            Connection() = default;
        private:
            friend EpollManager;
            enum State{
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

            // Delimiter
            //static constexpr char * addition_ = "\r\n";
            //static constexpr size_t addition_len_ = sizeof(addition_) - 1;

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

        static constexpr int max_events_{100};
        // Due no support in Ubuntu 16.04 (old epoll.h, but kernel support this flag)
        enum EPOLLFLAG{
            EPOLLEXCLUSIVE = (1u << 28)
        };

        // -1 means infinity time of event waiting
        static constexpr int max_timeout_{10000};

        Worker *worker_;
        std::unordered_set<int> connection_sockets_;
        std::unordered_map<int, Connection> connections_;
        epoll_event events_[max_events_];
        int epoll_fd_;

        //TODO: Make implementation without this field
        int server_socket_;
    };

    pthread_t thread_;
    std::atomic<bool> running_;
    std::shared_ptr<Afina::Storage> ps_;
    EpollManager *manager_;
};

} // namespace NonBlocking
} // namespace Network
} // namespace Afina
#endif // AFINA_NETWORK_NONBLOCKING_WORKER_H
