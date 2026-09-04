#pragma once
#include "platform_sockets.hpp"
#include <string>
#include <stdexcept>
#include <cstring>

namespace net {

// A minimal blocking TCP client socket, used by the Order Gateway (to
// reach the Matching Engine) and by the C++ trading client (to reach the
// Order Gateway). Builds on POSIX and native Windows via platform_sockets.hpp.
class TcpClient {
public:
    TcpClient(const std::string& host, int port) {
        fd_ = ::socket(AF_INET, SOCK_STREAM, 0);
        if (fd_ == kInvalidSocket) throw std::runtime_error("socket() failed");

        sockaddr_in addr{};
        addr.sin_family = AF_INET;
        addr.sin_port = htons(static_cast<unsigned short>(port));

        // Accept either a literal IP or a hostname (e.g. a Docker Compose
        // service name like "matching_engine"), resolving via getaddrinfo
        // when inet_pton can't parse it as a raw dotted-quad address.
        if (::inet_pton(AF_INET, host.c_str(), &addr.sin_addr) != 1) {
            addrinfo hints{};
            hints.ai_family = AF_INET;
            hints.ai_socktype = SOCK_STREAM;
            addrinfo* result = nullptr;
            if (::getaddrinfo(host.c_str(), nullptr, &hints, &result) != 0 || !result) {
                throw std::runtime_error("could not resolve host: " + host);
            }
            addr.sin_addr = reinterpret_cast<sockaddr_in*>(result->ai_addr)->sin_addr;
            ::freeaddrinfo(result);
        }

        if (::connect(fd_, reinterpret_cast<sockaddr*>(&addr), sizeof(addr)) < 0) {
            throw std::runtime_error("connect() failed to " + host + ":" + std::to_string(port));
        }
        int one = 1;
        setsockopt(fd_, IPPROTO_TCP, TCP_NODELAY, reinterpret_cast<const char*>(&one), sizeof(one));
    }

    ~TcpClient() { if (fd_ != kInvalidSocket) close_socket(fd_); }

    bool send_line(const std::string& line) {
        std::string out = line;
        if (out.empty() || out.back() != '\n') out.push_back('\n');
        size_t total = 0;
        while (total < out.size()) {
            int n = ::send(fd_, out.data() + total, static_cast<int>(out.size() - total), MSG_NOSIGNAL);
            if (n <= 0) return false;
            total += static_cast<size_t>(n);
        }
        return true;
    }

    // Reads a single newline-delimited message (blocking). Returns false on EOF/error.
    bool recv_line(std::string& out_line) {
        while (true) {
            size_t pos = buffer_.find('\n');
            if (pos != std::string::npos) {
                out_line = buffer_.substr(0, pos);
                buffer_.erase(0, pos + 1);
                return true;
            }
            char chunk[4096];
            int n = ::recv(fd_, chunk, sizeof(chunk), 0);
            if (n <= 0) return false;
            buffer_.append(chunk, static_cast<size_t>(n));
        }
    }

    socket_t fd() const { return fd_; }

private:
    socket_t fd_{kInvalidSocket};
    std::string buffer_;
};

} // namespace net
