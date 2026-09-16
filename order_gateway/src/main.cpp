// Order Gateway
// -------------
// Client-facing TCP server (default port 6000). Trading clients (Python or
// C++) connect here and send newline-delimited JSON orders. The gateway:
//   1. Performs basic request validation (required fields, sane types,
//      symbol allow-list, simple per-connection rate limiting).
//   2. Assigns each accepted order a gateway-level sequence number and
//      forwards it to the Matching Engine over a dedicated TCP connection.
//   3. Streams acks/fills/rejects from the engine straight back to the
//      client that originated the order.
//
// Every client connection gets its own outbound connection to the engine,
// which keeps response routing trivial (no correlation IDs needed) at the
// cost of one extra fd per client - a reasonable tradeoff for a reference
// implementation.

#include "tcp_server.hpp"
#include "tcp_client.hpp"
#include "../third_party/json.hpp"
#include <iostream>
#include <thread>
#include <atomic>
#include <unordered_set>
#include <chrono>

using json = nlohmann::json;

namespace {
std::atomic<uint64_t> g_gateway_seq{1};
const std::unordered_set<std::string> kAllowedSymbols = {
    "AAPL", "MSFT", "GOOG", "AMZN", "TSLA", "NVDA", "BTC-USD", "ETH-USD"
};
constexpr size_t kMaxOrdersPerSecondPerClient = 200;
}

struct RateLimiter {
    std::chrono::steady_clock::time_point window_start = std::chrono::steady_clock::now();
    size_t count = 0;

    bool allow() {
        auto now = std::chrono::steady_clock::now();
        if (now - window_start > std::chrono::seconds(1)) {
            window_start = now;
            count = 0;
        }
        if (count >= kMaxOrdersPerSecondPerClient) return false;
        ++count;
        return true;
    }
};

// Validates a raw client request. Returns an empty string if OK, otherwise
// a human-readable rejection reason.
std::string validate(const json& req) {
    std::string type = req.value("type", "");
    if (type == "cancel_order") {
        if (!req.contains("order_id")) return "cancel_order missing order_id";
        return "";
    }
    if (type != "new_order") return "unknown message type: " + type;

    if (!req.contains("symbol") || !req["symbol"].is_string())
        return "missing/invalid symbol";
    std::string symbol = req["symbol"];
    if (kAllowedSymbols.find(symbol) == kAllowedSymbols.end())
        return "symbol not tradable on this venue: " + symbol;

    if (!req.contains("side") || (req["side"] != "buy" && req["side"] != "sell"))
        return "side must be 'buy' or 'sell'";

    std::string order_type = req.value("order_type", "limit");
    if (order_type != "limit" && order_type != "market")
        return "order_type must be 'limit' or 'market'";

    if (!req.contains("qty") || !req["qty"].is_number() || req["qty"].get<double>() <= 0)
        return "qty must be a positive number";
    if (req["qty"].get<double>() > 1'000'000)
        return "qty exceeds max order size (1,000,000)";

    if (order_type == "limit") {
        if (!req.contains("price") || !req["price"].is_number() || req["price"].get<double>() <= 0)
            return "limit orders require a positive price";
    }
    return "";
}

int main(int argc, char** argv) {
    int client_port = 6000;
    std::string engine_host = "127.0.0.1";
    int engine_port = 5001;
    if (argc > 1) client_port = std::stoi(argv[1]);
    if (argc > 2) engine_host = argv[2];
    if (argc > 3) engine_port = std::stoi(argv[3]);

    std::cout << "[order_gateway] listening on :" << client_port
              << ", forwarding to matching_engine " << engine_host << ":" << engine_port
              << std::endl;

    net::TcpServer server(client_port);

    // Each client connection owns a RateLimiter and a dedicated engine link.
    struct ClientState {
        std::unique_ptr<net::TcpClient> engine_link;
        std::shared_ptr<RateLimiter> limiter = std::make_shared<RateLimiter>();
        std::thread reader_thread;
    };
    std::mutex states_mutex;
    std::unordered_map<net::Connection*, std::shared_ptr<ClientState>> states;

    server.on_connect([&](std::shared_ptr<net::Connection> conn) {
        auto state = std::make_shared<ClientState>();
        try {
            state->engine_link = std::make_unique<net::TcpClient>(engine_host, engine_port);
        } catch (const std::exception& e) {
            std::cerr << "[order_gateway] failed to reach matching engine: " << e.what() << std::endl;
            conn->send_line(json{{"type", "error"}, {"reason", "engine unavailable"}}.dump());
            conn->close_conn();
            return;
        }
        // Background thread: relay engine -> client
        net::TcpClient* link = state->engine_link.get();
        std::weak_ptr<net::Connection> weak_conn = conn;
        state->reader_thread = std::thread([link, weak_conn]() {
            std::string line;
            while (link->recv_line(line)) {
                auto c = weak_conn.lock();
                if (!c || c->is_closed()) break;
                c->send_line(line);
            }
        });
        state->reader_thread.detach();

        std::lock_guard<std::mutex> lock(states_mutex);
        states[conn.get()] = state;
        std::cout << "[order_gateway] client connected" << std::endl;
    });

    server.on_disconnect([&](std::shared_ptr<net::Connection> conn) {
        std::lock_guard<std::mutex> lock(states_mutex);
        states.erase(conn.get());
        std::cout << "[order_gateway] client disconnected" << std::endl;
    });

    server.on_line([&](std::shared_ptr<net::Connection> conn, const std::string& line) {
        std::shared_ptr<ClientState> state;
        {
            std::lock_guard<std::mutex> lock(states_mutex);
            auto it = states.find(conn.get());
            if (it == states.end()) return;
            state = it->second;
        }

        if (!state->limiter->allow()) {
            conn->send_line(json{{"type", "reject"}, {"reason", "rate limit exceeded"}}.dump());
            return;
        }

        json req;
        try {
            req = json::parse(line);
        } catch (const std::exception&) {
            conn->send_line(json{{"type", "reject"}, {"reason", "malformed JSON"}}.dump());
            return;
        }

        std::string err = validate(req);
        if (!err.empty()) {
            conn->send_line(json{{"type", "reject"},
                                  {"client_order_id", req.value("client_order_id", "")},
                                  {"reason", err}}.dump());
            return;
        }

        req["gateway_seq"] = g_gateway_seq.fetch_add(1);
        if (!state->engine_link->send_line(req.dump())) {
            conn->send_line(json{{"type", "reject"}, {"reason", "engine link down"}}.dump());
        }
    });

    server.run();
    return 0;
}
