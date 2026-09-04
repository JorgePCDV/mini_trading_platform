#pragma once
// platform_sockets.hpp
// ---------------------
// Isolates the (few) differences between POSIX sockets and Winsock so that
// tcp_server.hpp / tcp_client.hpp can use ordinary BSD-socket calls
// (socket, bind, listen, accept, connect, send, recv, setsockopt) verbatim
// on Linux, macOS, *and* native Windows (MSVC or MinGW) - no WSL required.
//
// What differs between the two platforms, and how this header papers over it:
//   - Socket handle type:      int (POSIX)         vs SOCKET (Windows)
//   - Invalid-socket sentinel: -1                  vs INVALID_SOCKET
//   - Closing a socket:        close()             vs closesocket()
//   - accept()'s addrlen type: socklen_t            vs int
//   - SIGPIPE suppression:     MSG_NOSIGNAL flag     vs not needed/defined
//   - Global init/teardown:    none needed           vs WSAStartup/WSACleanup
//     (required once per process before any Winsock call)

#ifdef _WIN32
  #ifndef WIN32_LEAN_AND_MEAN
    #define WIN32_LEAN_AND_MEAN
  #endif
  #ifndef NOMINMAX
    #define NOMINMAX
  #endif
  #include <winsock2.h>
  #include <ws2tcpip.h>
  #if defined(_MSC_VER)
    #pragma comment(lib, "ws2_32.lib")
  #endif

  namespace net {
  using socket_t = SOCKET;
  using socklen_compat_t = int;
  constexpr socket_t kInvalidSocket = INVALID_SOCKET;

  inline void close_socket(socket_t s) { ::closesocket(s); }
  inline void shutdown_both(socket_t s) { ::shutdown(s, SD_BOTH); }

  // Winsock needs WSAStartup() called once before any socket call, and
  // WSACleanup() at process exit. This runs it exactly once per process
  // regardless of how many translation units include this header, via a
  // C++17 inline variable (safe against duplicate-definition across TUs).
  struct WinsockInitializer {
      WinsockInitializer() {
          WSADATA wsa_data;
          WSAStartup(MAKEWORD(2, 2), &wsa_data);
      }
      ~WinsockInitializer() { WSACleanup(); }
  };
  inline WinsockInitializer g_winsock_initializer;
  } // namespace net

  #ifndef MSG_NOSIGNAL
    #define MSG_NOSIGNAL 0
  #endif

#else // POSIX (Linux, macOS)
  #include <sys/socket.h>
  #include <netinet/in.h>
  #include <netinet/tcp.h>
  #include <unistd.h>
  #include <arpa/inet.h>
  #include <netdb.h>
  #include <cerrno>

  namespace net {
  using socket_t = int;
  using socklen_compat_t = socklen_t;
  constexpr socket_t kInvalidSocket = -1;

  inline void close_socket(socket_t s) { ::close(s); }
  inline void shutdown_both(socket_t s) { ::shutdown(s, SHUT_RDWR); }
  } // namespace net
#endif
