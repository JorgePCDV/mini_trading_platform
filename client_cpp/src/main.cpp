// Trading Client (C++)
// ---------------------
// A native C++ counterpart to client/trading_client.py, demonstrating that
// the JSON-over-TCP protocol is language-agnostic (as depicted in the
// architecture diagram: "Trading Client - Python / C++").
//
// Connects to the Order Gateway, sends orders, and prints every ack/fill/
// reject that streams back on a background reader thread.
//
// Usage:
//   trading_client_cpp --client-id alice
//   trading_client_cpp --client-id bob --symbol AAPL --side buy --qty 10 --price 150.25
//
// Interactive commands once connected:
//   buy SYMBOL QTY PRICE
//   sell SYMBOL QTY PRICE
//   mktbuy SYMBOL QTY
//   mktsell SYMBOL QTY
//   quit

#include "tcp_client.hpp"
#include "json.hpp"
#include <iostream>
#include <sstream>
#include <thread>
#include <atomic>
#include <random>
#include <iomanip>

using json = nlohmann::json;

namespace {

std::string random_client_order_id() {
    static std::mt19937_64 rng(std::random_device{}());
    static const char alphabet[] = "0123456789abcdef";
    std::string id;
    for (int i = 0; i < 8; ++i) id += alphabet[rng() % 16];
    return id;
}

void print_message(const json& msg) {
    std::string type = msg.value("type", "");
    if (type == "ack") {
        std::cout << "  ACK    order_id=" << msg.value("order_id", 0)
                  << " coid=" << msg.value("client_order_id", "")
                  << " status=" << msg.value("status", "")
                  << " leaves=" << msg.value("leaves_qty", 0) << std::endl;
    } else if (type == "fill") {
        std::cout << "  FILL   order_id=" << msg.value("order_id", 0)
                  << " coid=" << msg.value("client_order_id", "")
                  << " " << msg.value("side", "") << " " << msg.value("qty", 0)
                  << " @ " << msg.value("price", 0.0) << std::endl;
    } else if (type == "reject") {
        std::cout << "  REJECT coid=" << msg.value("client_order_id", "")
                  << " reason=" << msg.value("reason", "") << std::endl;
    } else if (type == "cancel_ack") {
        std::cout << "  CANCEL_ACK order_id=" << msg.value("order_id", 0) << std::endl;
    } else {
        std::cout << "  " << type << ": " << msg.dump() << std::endl;
    }
}

struct Args {
    std::string host = "127.0.0.1";
    int port = 6000;
    std::string client_id = "cpp_client";
    std::string symbol;
    std::string side;
    double qty = 0;
    double price = 0;
    bool market = false;
    bool has_order = false;
};

Args parse_args(int argc, char** argv) {
    Args a;
    for (int i = 1; i < argc; ++i) {
        std::string arg = argv[i];
        auto next = [&]() -> std::string { return (i + 1 < argc) ? argv[++i] : ""; };
        if (arg == "--host") a.host = next();
        else if (arg == "--port") a.port = std::stoi(next());
        else if (arg == "--client-id") a.client_id = next();
        else if (arg == "--symbol") { a.symbol = next(); a.has_order = true; }
        else if (arg == "--side") a.side = next();
        else if (arg == "--qty") a.qty = std::stod(next());
        else if (arg == "--price") a.price = std::stod(next());
        else if (arg == "--market") a.market = true;
    }
    return a;
}

void send_new_order(net::TcpClient& link, const std::string& client_id, const std::string& symbol,
                     const std::string& side, bool market, double qty, double price) {
    json req{{"type", "new_order"},
             {"client_order_id", random_client_order_id()},
             {"client_id", client_id},
             {"symbol", symbol},
             {"side", side},
             {"order_type", market ? "market" : "limit"},
             {"qty", qty}};
    if (!market) req["price"] = price;
    link.send_line(req.dump());
}

} // namespace

int main(int argc, char** argv) {
    Args args = parse_args(argc, argv);

    net::TcpClient link(args.host, args.port);
    std::cout << "Connected to order gateway " << args.host << ":" << args.port
              << " as '" << args.client_id << "'" << std::endl;

    std::atomic<bool> running{true};
    std::thread reader([&]() {
        std::string line;
        while (running && link.recv_line(line)) {
            try {
                print_message(json::parse(line));
            } catch (const std::exception&) {
                std::cout << "  (unparseable message: " << line << ")" << std::endl;
            }
        }
    });

    if (args.has_order && !args.side.empty() && args.qty > 0) {
        std::cout << "Sending " << args.side << " " << args.qty << " " << args.symbol
                   << (args.market ? " MKT" : (" @ " + std::to_string(args.price)))
                   << " as " << args.client_id << std::endl;
        send_new_order(link, args.client_id, args.symbol, args.side, args.market, args.qty, args.price);
        std::this_thread::sleep_for(std::chrono::milliseconds(800));
        running = false;
        reader.detach();
        return 0;
    }

    std::cout << "Commands: buy SYMBOL QTY PRICE | sell SYMBOL QTY PRICE | "
                 "mktbuy SYMBOL QTY | mktsell SYMBOL QTY | quit" << std::endl;
    std::string line;
    while (std::cout << "> " && std::getline(std::cin, line)) {
        std::istringstream iss(line);
        std::string cmd;
        iss >> cmd;
        if (cmd == "quit") break;
        if (cmd == "buy" || cmd == "sell") {
            std::string symbol; double qty, price;
            if (iss >> symbol >> qty >> price) {
                send_new_order(link, args.client_id, symbol, cmd, false, qty, price);
            } else {
                std::cout << "  usage: " << cmd << " SYMBOL QTY PRICE" << std::endl;
            }
        } else if (cmd == "mktbuy" || cmd == "mktsell") {
            std::string symbol; double qty;
            std::string side = (cmd == "mktbuy") ? "buy" : "sell";
            if (iss >> symbol >> qty) {
                send_new_order(link, args.client_id, symbol, side, true, qty, 0.0);
            } else {
                std::cout << "  usage: " << cmd << " SYMBOL QTY" << std::endl;
            }
        } else if (!cmd.empty()) {
            std::cout << "  unrecognized command" << std::endl;
        }
    }

    running = false;
    reader.detach();
    return 0;
}
