#pragma once
#include "order.hpp"
#include <map>
#include <deque>
#include <unordered_map>
#include <vector>
#include <optional>
#include <functional>
#include <mutex>

namespace engine {

// Result of submitting an order into the book.
struct SubmitResult {
    bool accepted{true};
    std::string reject_reason;
    std::vector<Fill> fills;
    uint64_t remaining_qty{0};
};

// A single price level snapshot, used for market data / depth output.
struct PriceLevel {
    double price;
    uint64_t qty;
};

struct BookSnapshot {
    std::string symbol;
    std::vector<PriceLevel> bids; // best (highest) first
    std::vector<PriceLevel> asks; // best (lowest) first
};

// Price-time priority limit order book for a single symbol.
// Bids are kept in descending price order, asks in ascending price order.
// Within a price level, orders are FIFO (time priority) via a deque.
class OrderBook {
public:
    explicit OrderBook(std::string symbol) : symbol_(std::move(symbol)) {}

    // Adds a new order to the book, matching it against the opposite side
    // as much as possible. Any unmatched quantity of a Limit order rests
    // in the book. Market orders never rest - unmatched qty is cancelled.
    SubmitResult submit(Order order);

    // Cancels a resting order by engine order_id. Returns true if found & removed.
    bool cancel(uint64_t order_id, std::string* out_symbol = nullptr);

    BookSnapshot snapshot(size_t depth = 10) const;

    const std::string& symbol() const { return symbol_; }

    double last_trade_price() const { return last_trade_price_; }

private:
    struct Location {
        Side side;
        double price;
    };

    std::string symbol_;
    // bids: highest price first -> use std::greater
    std::map<double, std::deque<Order>, std::greater<double>> bids_;
    // asks: lowest price first -> default std::less
    std::map<double, std::deque<Order>, std::less<double>> asks_;

    // order_id -> {side, price} so we can find & erase in O(log n) for cancel
    std::unordered_map<uint64_t, Location> locations_;

    double last_trade_price_{0.0};
};

} // namespace engine
