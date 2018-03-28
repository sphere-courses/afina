#ifndef AFINA_NETWORK_NONBLOCKING_SERVER_H
#define AFINA_NETWORK_NONBLOCKING_SERVER_H

#include <vector>

#include <afina/network/Server.h>
#include "Worker.h"

namespace Afina {
namespace Network {
namespace NonBlocking {

// Forward declaration, see Worker.h
class Worker;

/**
 * # Network resource manager implementation
 * Epoll based server
 */
class ServerImpl : public Server {
public:
    explicit ServerImpl(std::shared_ptr<Afina::Storage> ps) noexcept ;

    ~ServerImpl() noexcept override = default ;

    // See Server.h
    void Start(uint16_t port, uint16_t workers) throw() override;

    // See Server.h
    void Stop() noexcept override;

    // See Server.h
    void Join() noexcept override;

    // See Server.h
    void AssignFifo(const std::string& fifo_read_file, const std::string& fifo_write_file) noexcept override;

private:
    // Port to listen for new connections, permits access only from
    // inside of accept_thread
    // Read-only
    uint16_t listen_port_;

    int server_socket_;

    std::string fifo_read_file_, fifo_write_file_;

    // Thread that is accepting new connections
    std::unordered_set<Worker *> workers_;
};

} // namespace NonBlocking
} // namespace Network
} // namespace Afina

#endif // AFINA_NETWORK_NONBLOCKING_SERVER_H
