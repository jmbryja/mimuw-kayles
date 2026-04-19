#include "common.hpp"
#include <iostream>
#include <charconv>
#include <unistd.h>
#include <sys/time.h>

extern "C" {
#include "err.h"
}

Socket::Socket(int domain, int type, int protocol) {
    fd = socket(domain, type, protocol);
    if (fd < 0) syserr("socket");
}

Socket::~Socket() {
    if (fd >= 0) close(fd);
}

void Socket::set_timeout(const int seconds) const {
    struct timeval tv;
    tv.tv_sec = seconds;
    tv.tv_usec = 0;
    if (setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv)) < 0) {
        syserr("setsockopt");
    }
}

uint16_t parse_port(std::string_view str) {
    uint16_t port;
    auto [ptr, ec] = std::from_chars(str.data(), str.data() + str.size(), port);
    if (ec != std::errc{} || ptr != str.data() + str.size()) {
        fatal("Invalid port: %.*s", static_cast<int>(str.size()), str.data());
    }
    return port;
}
int parse_timeout (const std::string& str) {
    int timeout;
    auto [ptr, ec] = std::from_chars(str.data(), str.data() + str.size(), timeout);
    if (ec != std::errc{} || ptr != str.data() + str.size()) {
        fatal("Invalid timeout: %.*s", static_cast<int>(str.size()), str.data());
    }
    return timeout;
}
std::string parse_address (const std::string& str) {
    if (str.empty())
        fatal("Invalid address");
    return str;
}