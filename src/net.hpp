#pragma once
#include <optional>

#include <netinet/in.h>

#include "util/fd.hpp"

auto create_sock_addr(uint32_t addr, uint16_t port) -> sockaddr_in;
auto create_udp_socket(uint32_t addr, uint16_t port) -> std::optional<FileDescriptor>;
