// Matching Engine
// -----------------
// Listens on two ports:
//   ORDER_PORT  (default 5001) - order intake from the Order Gateway.
//                Accepts newline-delimited JSON: new_order / cancel_order.
//                Sends back newline-delimited JSON: ack / reject / fill.
//   MD_PORT     (default 5002) - market data output.
//                Broadcasts newline-delimited JSON: trade / book_update
//                to every connected subscriber (Market Data Publisher role,
//                Risk Engine, Strategy Engine, Analytics all subscribe here).
//
// In a real deployment the "Order Gateway" and "Market Data Publisher"
// boxes in the architecture diagram would be separate processes; here the
// matching engine exposes the two ports directly and order_gateway (a
// separate binary in this repo) sits in front of ORDER_PORT to do client
// validation, auth and rate limiting before forwarding to the engine.

#include "order_book.hpp"
#include "tcp_server.hpp"
#include "../third_party/json.hpp"
#include <iostream>
#include <unordered_map>
#include <memory>
#include <mutex>
#include <thread>
#include <set>
#include <fstream>
#include <chrono>
#include <iomanip>
#include <sstream>

using json = nlohmann::json;
using namespace engine;

namespace {

std::mutex g_engine_mutex;
std::unordered_map<std::string, std::unique_ptr<OrderBook>> g_books;
std::atomic<uint64_t> g_order_seq{1};

// order_id -> which order-intake connection owns it, so cancels/acks route back.
std::unordered_map<uint64_t, std::weak_ptr<net::Connection>> g_owner;

// Market data subscriber connections.
std::mutex g_md_mutex;
std::set<std::shared_ptr<net::Connection>> g_md_subscribers;

std::ofstream g_trade_log;
std::mutex g_log_mutex;

OrderBook& book_for(const std::string& symbol) {
    auto it = g_books.find(symbol);
    if (it == g_books.end()) {
        auto [ins_it, ok] = g_books.emplace(symbol, std::make_unique<OrderBook>(symbol));
        it = ins_it;
    }
    return *it->second;
}

void broadcast_market_data(const json& msg) {
    std::string line = msg.dump();
    std::lock_guard<std::mutex> lock(g_md_mutex);
    for (auto it = g_md_subscribers.begin(); it != g_md_subscribers.end();) {
        if ((*it)->is_closed() || !(*it)->send_line(line)) {
            it = g_md_subscribers.erase(it);
        } else {
            ++it;
        }
    }
}

void publish_book_update(const std::string& symbol) {
    std::lock_guard<std::mutex> lock(g_engine_mutex);
    auto snap = book_for(symbol).snapshot(10);
    json j;
    j["type"] = "book_update";
    j["symbol"] = symbol;
    j["ts"] = now_nanos();
    for (auto& lvl : snap.bids) j["bids"].push_back({lvl.price, lvl.qty});
    for (auto& lvl : snap.asks) j["asks"].push_back({lvl.price, lvl.qty});
    broadcast_market_data(j);
}

void log_trade(const Fill& f) {
    std::lock_guard<std::mutex> lock(g_log_mutex);
    if (!g_trade_log.is_open()) return;
    g_trade_log << f.ts_ns << "," << f.symbol << "," << f.price << "," << f.qty << ","
                << side_to_string(f.aggressor_side) << "," << f.aggressor_client_id << ","
                << f.resting_client_id << "\n";
    g_trade_log.flush();
}

void handle_new_order(std::shared_ptr<net::Connection> conn, const json& req) {
    Order o;
    o.order_id = g_order_seq.fetch_add(1);
    o.client_order_id = req.value("client_order_id", std::string(""));
    o.client_id = req.value("client_id", std::string("unknown"));
    o.symbol = req.value("symbol", std::string(""));
    o.side = side_from_string(req.value("side", std::string("buy")));
    o.type = type_from_string(req.value("order_type", std::string("limit")));
    o.price = req.value("price", 0.0);
    o.qty = req.value("qty", (uint64_t)0);
    o.ts_ns = now_nanos();

    if (o.symbol.empty()) {
        json rej{{"type", "reject"}, {"client_order_id", o.client_order_id},
                 {"reason", "missing symbol"}};
        conn->send_line(rej.dump());
        return;
    }

    SubmitResult res;
    {
        std::lock_guard<std::mutex> lock(g_engine_mutex);
        res = book_for(o.symbol).submit(o);
        if (res.accepted && res.remaining_qty > 0 && o.type == OrderType::Limit) {
            g_owner[o.order_id] = conn;
        }
    }

    if (!res.accepted) {
        json rej{{"type", "reject"}, {"client_order_id", o.client_order_id},
                 {"reason", res.reject_reason}};
        conn->send_line(rej.dump());
        return;
    }

    json ack{{"type", "ack"},
             {"order_id", o.order_id},
             {"client_order_id", o.client_order_id},
             {"symbol", o.symbol},
             {"status", res.remaining_qty == o.qty && res.fills.empty() ? "resting" : "accepted"},
             {"leaves_qty", res.remaining_qty}};
    conn->send_line(ack.dump());

    for (const auto& f : res.fills) {
        json fill_agg{{"type", "fill"},
                      {"exec_id", f.exec_id},
                      {"order_id", f.aggressor_order_id},
                      {"client_order_id", f.aggressor_client_order_id},
                      {"symbol", f.symbol},
                      {"side", side_to_string(f.aggressor_side)},
                      {"price", f.price},
                      {"qty", f.qty},
                      {"ts", f.ts_ns}};
        conn->send_line(fill_agg.dump());

        // notify the resting order's owning connection too, if still connected
        std::shared_ptr<net::Connection> resting_conn;
        {
            std::lock_guard<std::mutex> lock(g_engine_mutex);
            auto it = g_owner.find(f.resting_order_id);
            if (it != g_owner.end()) resting_conn = it->second.lock();
        }
        if (resting_conn) {
            json fill_rest{{"type", "fill"},
                           {"exec_id", f.exec_id},
                           {"order_id", f.resting_order_id},
                           {"client_order_id", f.resting_client_order_id},
                           {"symbol", f.symbol},
                           {"side", side_to_string(f.aggressor_side == Side::Buy ? Side::Sell : Side::Buy)},
                           {"price", f.price},
                           {"qty", f.qty},
                           {"ts", f.ts_ns}};
            resting_conn->send_line(fill_rest.dump());
        }

        json trade_md{{"type", "trade"},
                      {"symbol", f.symbol},
                      {"price", f.price},
                      {"qty", f.qty},
                      {"aggressor_side", side_to_string(f.aggressor_side)},
                      {"ts", f.ts_ns}};
        broadcast_market_data(trade_md);
        log_trade(f);
    }

    if (!res.fills.empty() || res.remaining_qty != o.qty) {
        publish_book_update(o.symbol);
    } else if (o.type == OrderType::Limit) {
        publish_book_update(o.symbol);
    }
}

void handle_cancel(std::shared_ptr<net::Connection> conn, const json& req) {
    uint64_t order_id = req.value("order_id", (uint64_t)0);
    std::string symbol;
    bool ok;
    {
        std::lock_guard<std::mutex> lock(g_engine_mutex);
        ok = book_for(req.value("symbol", std::string(""))).cancel(order_id, &symbol);
        if (ok) g_owner.erase(order_id);
    }
    json resp{{"type", ok ? "cancel_ack" : "cancel_reject"}, {"order_id", order_id}};
    conn->send_line(resp.dump());
    if (ok) publish_book_update(symbol);
}

} // namespace

int main(int argc, char** argv) {
    int order_port = 5001;
    int md_port = 5002;
    if (argc > 1) order_port = std::stoi(argv[1]);
    if (argc > 2) md_port = std::stoi(argv[2]);

    g_trade_log.open("logs/trades.csv", std::ios::app);
    if (g_trade_log.tellp() == 0) {
        g_trade_log << "ts_ns,symbol,price,qty,aggressor_side,aggressor_client,resting_client\n";
    }

    std::cout << "[matching_engine] order intake on :" << order_port
              << ", market data on :" << md_port << std::endl;

    net::TcpServer md_server(md_port);
    md_server.on_connect([](std::shared_ptr<net::Connection> conn) {
        std::lock_guard<std::mutex> lock(g_md_mutex);
        g_md_subscribers.insert(conn);
        std::cout << "[matching_engine] market data subscriber connected" << std::endl;
    });
    md_server.on_disconnect([](std::shared_ptr<net::Connection> conn) {
        std::lock_guard<std::mutex> lock(g_md_mutex);
        g_md_subscribers.erase(conn);
    });
    md_server.on_line([](std::shared_ptr<net::Connection>, const std::string&) {
        // subscribers are read-only; ignore any inbound data
    });
    std::thread md_thread([&md_server]() { md_server.run(); });

    net::TcpServer order_server(order_port);
    order_server.on_connect([](std::shared_ptr<net::Connection>) {
        std::cout << "[matching_engine] order gateway connected" << std::endl;
    });
    order_server.on_line([](std::shared_ptr<net::Connection> conn, const std::string& line) {
        try {
            json req = json::parse(line);
            std::string type = req.value("type", "");
            if (type == "new_order") {
                handle_new_order(conn, req);
            } else if (type == "cancel_order") {
                handle_cancel(conn, req);
            } else if (type == "snapshot_request") {
                publish_book_update(req.value("symbol", ""));
            }
        } catch (const std::exception& e) {
            json err{{"type", "error"}, {"reason", e.what()}};
            conn->send_line(err.dump());
        }
    });
    order_server.run(); // blocks main thread
    md_thread.join();
    return 0;
}
