#include <netinet/in.h>

#include "macros/assert.hpp"
#include "net.hpp"

auto get_peer_addr(const int fd) -> std::optional<uint32_t> {
    auto addr = sockaddr_storage();
    auto len  = socklen_t(sizeof(addr));
    ensure(getpeername(fd, (sockaddr*)&addr, &len) == 0, "errno={}({})", errno, strerror(errno));
    if(addr.ss_family == AF_INET) {
        return ntohl(((sockaddr_in*)&addr)->sin_addr.s_addr);
    } else {
        bail("unimplemented address family {}", addr.ss_family);
    }
}

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

