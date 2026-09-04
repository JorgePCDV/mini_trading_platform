#pragma once
#include "platform_sockets.hpp"
#include <string>
#include <functional>
#include <thread>
#include <vector>
#include <mutex>
#include <atomic>
#include <memory>
#include <stdexcept>
#include <cstring>

namespace net {

// A very small line-oriented (newline-delimited) TCP server.
// For each connection, `on_line` is invoked for every '\n'-terminated
// message received. `on_connect`/`on_disconnect` let callers track clients
// (e.g. to broadcast market data). Everything runs on background threads;
// `send_line` is thread-safe per connection via an internal mutex.
//
// Builds on POSIX (Linux/macOS) and native Windows (MSVC/MinGW) via
// platform_sockets.hpp - no WSL or POSIX emulation layer required.
class Connection {
public:
    explicit Connection(socket_t fd) : fd_(fd) {}
    ~Connection() { close_conn(); }

    bool send_line(const std::string& line) {
        std::lock_guard<std::mutex> lock(write_mutex_);
        if (closed_) return false;
        std::string out = line;
        if (out.empty() || out.back() != '\n') out.push_back('\n');
        size_t total = 0;
        while (total < out.size()) {
            int n = ::send(fd_, out.data() + total, static_cast<int>(out.size() - total), MSG_NOSIGNAL);
            if (n <= 0) { closed_ = true; return false; }
            total += static_cast<size_t>(n);
        }
        return true;
    }

    socket_t fd() const { return fd_; }
    bool is_closed() const { return closed_; }
    void close_conn() {
        bool expected = false;
        if (closed_.compare_exchange_strong(expected, true)) {
            shutdown_both(fd_);
            close_socket(fd_);
        }
    }

private:
    socket_t fd_;
    std::mutex write_mutex_;
    std::atomic<bool> closed_{false};
};

class TcpServer {
public:
    using LineHandler = std::function<void(std::shared_ptr<Connection>, const std::string&)>;
    using ConnHandler = std::function<void(std::shared_ptr<Connection>)>;

    TcpServer(int port) : port_(port) {}

    void on_line(LineHandler h) { on_line_ = std::move(h); }
    void on_connect(ConnHandler h) { on_connect_ = std::move(h); }
    void on_disconnect(ConnHandler h) { on_disconnect_ = std::move(h); }

    // Blocks forever accepting connections, spawning one thread per client.
    void run() {
        socket_t listen_fd = ::socket(AF_INET, SOCK_STREAM, 0);
        if (listen_fd == kInvalidSocket) {
            throw std::runtime_error("socket() failed");
        }
        int opt = 1;
        setsockopt(listen_fd, SOL_SOCKET, SO_REUSEADDR,
                   reinterpret_cast<const char*>(&opt), sizeof(opt));

        sockaddr_in addr{};
        addr.sin_family = AF_INET;
        addr.sin_addr.s_addr = INADDR_ANY;
        addr.sin_port = htons(static_cast<unsigned short>(port_));

        if (::bind(listen_fd, reinterpret_cast<sockaddr*>(&addr), sizeof(addr)) < 0) {
            throw std::runtime_error("bind() failed on port " + std::to_string(port_));
        }
        if (::listen(listen_fd, 128) < 0) {
            throw std::runtime_error("listen() failed on port " + std::to_string(port_));
        }

        while (true) {
            sockaddr_in client_addr{};
            socklen_compat_t len = sizeof(client_addr);
            socket_t client_fd = ::accept(listen_fd, reinterpret_cast<sockaddr*>(&client_addr), &len);
            if (client_fd == kInvalidSocket) continue;
            int one = 1;
            setsockopt(client_fd, IPPROTO_TCP, TCP_NODELAY,
                       reinterpret_cast<const char*>(&one), sizeof(one));

            auto conn = std::make_shared<Connection>(client_fd);
            std::thread(&TcpServer::client_loop, this, conn).detach();
        }
    }

private:
    void client_loop(std::shared_ptr<Connection> conn) {
        if (on_connect_) on_connect_(conn);
        std::string buffer;
        char chunk[4096];
        while (!conn->is_closed()) {
            int n = ::recv(conn->fd(), chunk, sizeof(chunk), 0);
            if (n <= 0) break;
            buffer.append(chunk, static_cast<size_t>(n));
            size_t pos;
            while ((pos = buffer.find('\n')) != std::string::npos) {
                std::string line = buffer.substr(0, pos);
                buffer.erase(0, pos + 1);
                if (!line.empty() && on_line_) on_line_(conn, line);
            }
        }
        conn->close_conn();
        if (on_disconnect_) on_disconnect_(conn);
    }

    int port_;
    LineHandler on_line_;
    ConnHandler on_connect_;
    ConnHandler on_disconnect_;
};

} // namespace net
