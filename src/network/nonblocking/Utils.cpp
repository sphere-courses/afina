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

void close_socket(int socket){
    shutdown(socket, SHUT_RDWR);
    close(socket);
}

ssize_t WriteStrict_NonBlock(int socket, const char *source, size_t len){
    ssize_t wrote_now{0};
    size_t wrote{0};
    while(wrote < len){
        // MSG_DONTWAIT is omitted due to nonblocking socket
        if((wrote_now = send(socket, source + wrote, len - wrote, 0)) == -1){
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

ssize_t ReadStrict_NonBlock(int socket, char *dest, size_t len){
    ssize_t read = 0;
    ssize_t read_now = 0;
    while (read < len) {
        // MSG_DONTWAIT is omitted due to nonblocking socket
        read_now = recv(socket, dest + read, len - read, 0);
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


} // namespace NonBlocking
} // namespace Network
} // namespace Afina
