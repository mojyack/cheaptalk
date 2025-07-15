#include <cstring>

#include <netinet/in.h>

#include "macros/assert.hpp"
#include "net.hpp"

auto create_sock_addr(const uint32_t addr, const uint16_t port) -> sockaddr_in {
    auto sockaddr            = sockaddr_in();
    sockaddr.sin_family      = AF_INET;
    sockaddr.sin_port        = htons(port);
    sockaddr.sin_addr.s_addr = htonl(addr);
    return sockaddr;
}

auto create_udp_socket(const uint32_t addr, const uint16_t port) -> std::optional<FileDescriptor> {
    auto sock = FileDescriptor(socket(AF_INET, SOCK_DGRAM, 0));
    ensure(sock.as_handle() >= 0);
    auto sockaddr = create_sock_addr(addr, port);
    ensure(bind(sock.as_handle(), (struct sockaddr*)&sockaddr, sizeof(sockaddr)) == 0, "errno={}({})", errno, strerror(errno));
    return sock;
}

