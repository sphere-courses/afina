#ifndef AFINA_NETWORK_NONBLOCKING_UTILS_H
#define AFINA_NETWORK_NONBLOCKING_UTILS_H

#include <cstring>
#include <stdio.h>

namespace Afina {
namespace Network {
namespace NonBlocking {

void make_socket_non_blocking(int sfd);

void close_socket(int socket);

ssize_t WriteStrict_NonBlock(int socket, const char *source, size_t len);

ssize_t ReadStrict_NonBlock(int socket, char *dest, size_t len);

bool IsConnectionActive(int socket);


} // namespace NonBlocking
} // namespace Network
} // namespace Afina

#endif // AFINA_NETWORK_NONBLOCKING_UTILS_H
