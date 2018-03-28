#include "Utils.h"

#include <stdexcept>

#include <fcntl.h>
#include <sys/socket.h>
#include <unistd.h>
#include <iostream>

namespace Afina {
namespace Network {
namespace NonBlocking {

void make_socket_non_blocking(int sfd) {
    int flags, s;

    flags = fcntl(sfd, F_GETFL, 0);
    if (flags == -1) {
        throw std::runtime_error("Failed to call fcntl to get socket flags");
    }

    flags |= O_NONBLOCK;
    s = fcntl(sfd, F_SETFL, flags);
    if (s == -1) {
        throw std::runtime_error("Failed to call fcntl to set socket flags");
    }
}


} // namespace NonBlocking
} // namespace Network
} // namespace Afina
