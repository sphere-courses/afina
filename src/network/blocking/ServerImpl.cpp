#include "ServerImpl.h"

#include <cstring>
#include <iostream>
#include <cmath>

#include <csignal>

#include <netdb.h>

#include <arpa/inet.h>
#include <unistd.h>

#include <afina/Storage.h>
#include <afina/execute/Command.h>
#include <protocol/Parser.h>
#include <sstream>

namespace Afina {
namespace Network {
namespace Blocking {

// See Server.h
ServerImpl::ServerImpl(std::shared_ptr<Afina::Storage> ps) : Server(ps) {}

// See Server.h
void ServerImpl::Start(uint32_t port, uint16_t n_workers) {
    std::cout << "network debug: " << __PRETTY_FUNCTION__ << std::endl;

    // If a client closes a connection, this will generally produce a SIGPIPE
    // signal that will kill the process. We want to ignore this signal, so send()
    // just returns -1 when this happens.
    sigset_t sig_mask;
    sigemptyset(&sig_mask);
    sigaddset(&sig_mask, SIGPIPE);
    if (pthread_sigmask(SIG_BLOCK, &sig_mask, nullptr) != 0) {
        throw std::runtime_error("Unable to mask SIGPIPE");
    }

    // Setup server parameters BEFORE thread created, that will guarantee
    // variable value visibility
    max_workers = n_workers;
    listen_port = port;

    // The pthread_create function creates a new thread.
    //
    // The first parameter is a pointer to a pthread_t variable, which we can use
    // in the remainder of the program to manage this thread.
    //
    // The second parameter is used to specify the attributes of this new thread
    // (e.g., its stack size). We can leave it NULL here.
    //
    // The third parameter is the function this thread will run. This function *must*
    // have the following prototype:
    //    void *f(void *args);
    //
    // Note how the function expects a single parameter of type void*. We are using it to
    // pass this pointer in order to proxy call to the class member function. The fourth
    // parameter to pthread_create is used to specify this parameter value.
    //
    // The thread we are creating here is the "server thread", which will be
    // responsible for listening on port 23300 for incoming connections. This thread,
    // in turn, will spawn threads to service each incoming connection, allowing
    // multiple clients to connect simultaneously.
    // Note that, in this particular example, creating a "server thread" is redundant,
    // since there will only be one server thread, and the program's main thread (the
    // one running main()) could fulfill this purpose.
    running.store(true);
    if (pthread_create(&accept_thread, nullptr, ServerImpl::RunAcceptorProxy, this) != 0) {
        throw std::runtime_error("Could not create server thread");
    }
}

// See Server.h
void ServerImpl::Stop() {
    std::cout << "network debug: " << __PRETTY_FUNCTION__ << std::endl;
    running.store(false);
}

// See Server.h
void ServerImpl::Join() {
    std::cout << "network debug: " << __PRETTY_FUNCTION__ << std::endl;

    pthread_join(accept_thread, nullptr);
}

// See Server.h
void *ServerImpl::RunAcceptorProxy(void *p) {
    auto srv = reinterpret_cast<ServerImpl *>(p);
    try {
        srv->RunAcceptor();
    } catch (std::runtime_error &ex) {
        std::cerr << "Server fails: " << ex.what() << std::endl;
    }
    return nullptr;
}

// See Server.h
void *ServerImpl::RunConnectionProxy(void *proxy_args) {
    auto *args = reinterpret_cast<ServerImpl::ProxyArgs*>(proxy_args);
    try{
        args->server->RunConnection(args->con_socket);
    } catch (std::runtime_error &ex) {
        std::cerr << "Connection fails: " << ex.what() << std::endl;
    }
    return nullptr;
}

// See Server.h
void ServerImpl::RunAcceptor() {
    std::cout << "network debug: " << __PRETTY_FUNCTION__ << std::endl;

    // For IPv4 we use struct sockaddr_in:
    // struct sockaddr_in {
    //     short int          sin_family;  // Address family, AF_INET
    //     unsigned short int sin_port;    // Port number
    //     struct in_addr     sin_addr;    // Internet address
    //     unsigned char      sin_zero[8]; // Same size as struct sockaddr
    // };
    //
    // Note we need to convert the port to network order

    struct sockaddr_in server_addr;
    std::memset(&server_addr, 0, sizeof(server_addr));
    server_addr.sin_family = AF_INET;          // IPv4
    server_addr.sin_port = htons(listen_port); // TCP port number
    server_addr.sin_addr.s_addr = INADDR_ANY;  // Bind to any address

    // Arguments are:
    // - Family: IPv4
    // - Type: Full-duplex stream (reliable)
    // - Protocol: TCP
    int server_socket = socket(PF_INET, SOCK_STREAM, IPPROTO_TCP);
    if (server_socket == -1) {
        throw std::runtime_error("Failed to open socket");
    }

    // when the server closes the socket,the connection must stay in the TIME_WAIT state to
    // make sure the client received the acknowledgement that the connection has been terminated.
    // During this time, this port is unavailable to other processes, unless we specify this option
    //
    // This option let kernel knows that we are OK that multiple threads/processes are listen on the
    // same port. In a such case kernel will balance input traffic between all listeners (except those who
    // are closed already)
    int opts = 1;
    if (setsockopt(server_socket, SOL_SOCKET, SO_REUSEADDR, &opts, sizeof(opts)) == -1) {
        close(server_socket);
        throw std::runtime_error("Socket setsockopt() failed");
    }

    // Bind the socket to the address. In other words let kernel know data for what address we'd
    // like to see in the socket
    if (bind(server_socket, (struct sockaddr *)&server_addr, sizeof(server_addr)) == -1) {
        close(server_socket);
        throw std::runtime_error("Socket bind() failed");
    }

    // Start listening. The second parameter is the "backlog", or the maximum number of
    // connections that we'll allow to queue up. Note that listen() doesn't block until
    // incoming connections arrive. It just makesthe OS aware that this process is willing
    // to accept connections on this socket (which is bound to a specific IP and port)
    if (listen(server_socket, 5) == -1) {
        close(server_socket);
        throw std::runtime_error("Socket listen() failed");
    }

    int client_socket;
    struct sockaddr_in client_addr;
    socklen_t sinSize = sizeof(struct sockaddr_in);
    while (running.load()) {
        std::cout << "network debug: waiting for connection..." << std::endl;

        // When an incoming connection arrives, accept it. The call to accept() blocks until
        // the incoming connection arrives
        if ((client_socket = accept(server_socket, (struct sockaddr *)&client_addr, &sinSize)) == -1) {
            close(server_socket);
            throw std::runtime_error("Socket accept() failed");
        }


        connections_mutex.lock();
        if(connections.size() + 1 > max_workers){
            // TODO: Send msg to client signalising limit overhead
            std::cout << "Connection number limit reached" << std::endl;
            close(client_socket);
        } else {
            pthread_t new_thread;
            ServerImpl::ProxyArgs args = {this, client_socket};

            if (pthread_create(&new_thread, nullptr, ServerImpl::RunConnectionProxy, &args) != 0) {
                // TODO: Don`t stop server if connection failed
                close(server_socket);
                close(client_socket);
                throw std::runtime_error("Could not create connection thread");
            }
            connections.insert(new_thread);
        }
        // TODO: Is it the right place for unlock?
        connections_mutex.unlock();

        // Default behavior
        /*
        // TODO: Start new thread and process data from/to connection
        {
            std::string msg = "TODO: start new thread and process memcached protocol instead";
            if (send(client_socket, msg.data(), msg.size(), 0) <= 0) {
                close(client_socket);
                close(server_socket);
                throw std::runtime_error("Socket send() failed");
            }
            close(client_socket);
        }
        */
    }

    // Cleanup on exit...
    close(server_socket);
}

// See Server.h
void ServerImpl::RunConnection(int con_socket) {
    std::cout << "network debug: " << __PRETTY_FUNCTION__ << std::endl;

    // TODO: Fix this code
    /*
    socklen_t sockop_out_len;
    if(getsockopt(con_socket, SOL_SOCKET, SO_RCVBUF, &max_buffer_size, &sockop_out_len) != 0){
        close(con_socket);
        // TODO: cout vs cerr vs something else
        std::cout << "Could not get buffer size; Error: " << std::string(strerror(errno)) << std::endl;
        return;
    }
    */

    // TODO: Better solution for max_***_size
    int max_buffer_size, max_data_size;
    max_buffer_size = 1024;
    max_data_size = 1024;

    // Delimiter
    constexpr char addition[] = "\r\n";
    constexpr size_t addition_len = sizeof(addition) - 1;
    
    char buffer[max_buffer_size];
    char data_block[max_data_size];

    size_t current_buffer_size, parsed;
    current_buffer_size = 0;
    parsed = 0;

    Protocol::Parser parser;
    std::stringstream server_ans_stream;
    std::string server_ans;

    while(running.load()){

        parser.Reset();
        server_ans_stream.clear();
        server_ans.clear();

        try{
            size_t parsed_now = 0;
            ssize_t read_now = 0;
            while(!parser.Parse(buffer + parsed, current_buffer_size - parsed, parsed_now)){
                parsed += parsed_now;
                if(current_buffer_size == max_buffer_size){
                    parsed = 0;
                    current_buffer_size = 0;
                }
                read_now = recv(con_socket, buffer + current_buffer_size, max_buffer_size - current_buffer_size, 0);
                if(read_now == -1){
                    // TODO: << "SERVER_ERROR " << e.what() << '\r' << '\n';cout vs cerr vs something else
                    std::cout << "recv error: " << std::string(strerror(errno)) << std::endl;
                    CloseConnection(con_socket);
                } else if (read_now == 0){
                    // TODO: Check if zero return value means socket shutdown
                    CheckConnection(con_socket);
                } else {
                    current_buffer_size += read_now;
                }
            }
            parsed += parsed_now;

            uint32_t body_size;
            std::unique_ptr<Execute::Command> command(parser.Build(body_size));
            if(body_size > max_data_size){
                throw std::runtime_error("Too long data_block");
            } else if(body_size > 0) {
                size_t current_data_size = std::min(current_buffer_size - parsed, static_cast<size_t >(body_size));
                current_buffer_size = current_buffer_size - parsed - current_data_size;
                std::memcpy(data_block, buffer + parsed, current_data_size);
                std::memmove(buffer, buffer + parsed + current_data_size, current_buffer_size);

                ReadStrict(con_socket, data_block + current_data_size, body_size - current_data_size);

                if(current_buffer_size < addition_len) {
                    ReadStrict(con_socket, buffer + current_buffer_size, addition_len - current_buffer_size);
                    current_buffer_size = addition_len;
                }

                if(strncmp(buffer, addition, addition_len) != 0) {
                    throw std::runtime_error("Incorrect command format");
                }
                parsed = addition_len;
            } else {
                current_buffer_size = current_buffer_size - parsed;
                memmove(buffer, buffer + parsed, current_buffer_size);
                parsed = 0;
            }

            command->Execute(*this->pStorage, std::string(data_block, body_size), server_ans);
            WriteStrict(con_socket, server_ans.data(), server_ans.size());
        } catch (std::exception &e){
            std::cout << "SERVER_ERROR " << e.what() << std::endl;
            server_ans_stream << "SERVER_ERROR " << e.what() << '\r' << '\n';
            server_ans = server_ans_stream.str();
            WriteStrict(con_socket, server_ans.data(), server_ans.size());
            current_buffer_size = 0;
            parsed = 0;
        }
    }
    CloseConnection(con_socket);
}

// See Server.h
void ServerImpl::ReadStrict(int con_socket, char *dest, size_t len){
    ssize_t read = 0;
    ssize_t read_now = 0;
    while (read < len) {
        read_now = recv(con_socket, dest + read, len - read, 0);
        if (read_now == -1) {
            // TODO: cout vs cerr vs something else
            std::cout << "resv error: " << std::string(strerror(errno)) << std::endl;
            CloseConnection(con_socket);
            return;
        } else if(read_now == 0){
            // TODO: Check if zero return value means socket shutdown
            CheckConnection(con_socket);
        } else {
            read += read_now;
        }
    }
}

// See Server.h
void ServerImpl::WriteStrict(int con_socket, const char *source, size_t len){
    ssize_t sent = 0;
    ssize_t send_now = 0;
    
    while(sent < len) {
        send_now = send(con_socket, source + sent, len - sent, 0);
        if (send_now == -1) {
            // TODO: cout vs cerr vs something else
            std::cout << "send error: " << std::string(strerror(errno)) << std::endl;
            CloseConnection(con_socket);
        } else {
            sent += send_now;
        }
    }
}

// See Server.h
void ServerImpl::CheckConnection(int con_socket) {
    // TODO: Find out better way to determine if the connection closed
    char test;
    if(send(con_socket, &test, 1, MSG_NOSIGNAL) == -1){
        CloseConnection(con_socket);
    }
}

// See Server.h
void ServerImpl::CloseConnection(int con_socket){
    pthread_t self_id = pthread_self();
    // TODO: Add more flexible shutdown()
    close(con_socket);

    connections_mutex.lock();
    connections.erase(self_id);
    connections_mutex.unlock();

    pthread_exit(nullptr);
}

} // namespace Blocking
} // namespace Network
} // namespace Afina
