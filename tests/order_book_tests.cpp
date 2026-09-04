// Lightweight unit tests for engine::OrderBook - no external framework,
// just asserts with descriptive output. Run via `ctest` or directly.
#include "order_book.hpp"
#include <cassert>
#include <iostream>

using namespace engine;

static int g_failures = 0;

#define CHECK(cond) \
    do { if (!(cond)) { std::cerr << "FAIL: " << #cond << " at " << __FILE__ << ":" << __LINE__ << "\n"; ++g_failures; } } while (0)

Order make_order(uint64_t id, Side side, OrderType type, double price, uint64_t qty) {
    Order o;
    o.order_id = id;
    o.client_id = "test";
    o.symbol = "AAPL";
    o.side = side;
    o.type = type;
    o.price = price;
    o.qty = qty;
    o.ts_ns = now_nanos();
    return o;
}

void test_resting_order_no_match() {
    OrderBook book("AAPL");
    auto res = book.submit(make_order(1, Side::Buy, OrderType::Limit, 100.0, 10));
    CHECK(res.accepted);
    CHECK(res.fills.empty());
    CHECK(res.remaining_qty == 10);
    auto snap = book.snapshot();
    CHECK(snap.bids.size() == 1);
    CHECK(snap.bids[0].price == 100.0);
    CHECK(snap.bids[0].qty == 10);
}

void test_simple_full_match() {
    OrderBook book("AAPL");
    book.submit(make_order(1, Side::Sell, OrderType::Limit, 100.0, 10));
    auto res = book.submit(make_order(2, Side::Buy, OrderType::Limit, 100.0, 10));
    CHECK(res.fills.size() == 1);
    CHECK(res.fills[0].qty == 10);
    CHECK(res.fills[0].price == 100.0);
    CHECK(res.remaining_qty == 0);
    auto snap = book.snapshot();
    CHECK(snap.bids.empty());
    CHECK(snap.asks.empty());
}

void test_partial_match_leaves_remainder() {
    OrderBook book("AAPL");
    book.submit(make_order(1, Side::Sell, OrderType::Limit, 100.0, 5));
    auto res = book.submit(make_order(2, Side::Buy, OrderType::Limit, 100.0, 10));
    CHECK(res.fills.size() == 1);
    CHECK(res.fills[0].qty == 5);
    CHECK(res.remaining_qty == 5);
    auto snap = book.snapshot();
    CHECK(snap.bids.size() == 1);
    CHECK(snap.bids[0].qty == 5);
}

void test_price_priority() {
    // Two resting sell orders at different prices; buy should match the
    // cheaper one first regardless of insertion order.
    OrderBook book("AAPL");
    book.submit(make_order(1, Side::Sell, OrderType::Limit, 101.0, 10));
    book.submit(make_order(2, Side::Sell, OrderType::Limit, 100.0, 10));
    auto res = book.submit(make_order(3, Side::Buy, OrderType::Limit, 101.0, 10));
    CHECK(res.fills.size() == 1);
    CHECK(res.fills[0].price == 100.0); // best (lowest) ask fills first
    CHECK(res.fills[0].resting_order_id == 2);
}

void test_time_priority_fifo_same_price() {
    OrderBook book("AAPL");
    book.submit(make_order(1, Side::Sell, OrderType::Limit, 100.0, 5)); // first in
    book.submit(make_order(2, Side::Sell, OrderType::Limit, 100.0, 5)); // second in
    auto res = book.submit(make_order(3, Side::Buy, OrderType::Limit, 100.0, 5));
    CHECK(res.fills.size() == 1);
    CHECK(res.fills[0].resting_order_id == 1); // FIFO: earlier order fills first
}

void test_market_order_sweeps_book() {
    OrderBook book("AAPL");
    book.submit(make_order(1, Side::Sell, OrderType::Limit, 100.0, 5));
    book.submit(make_order(2, Side::Sell, OrderType::Limit, 101.0, 5));
    auto res = book.submit(make_order(3, Side::Buy, OrderType::Market, 0.0, 10));
    CHECK(res.fills.size() == 2);
    CHECK(res.fills[0].price == 100.0);
    CHECK(res.fills[1].price == 101.0);
    CHECK(res.remaining_qty == 0);
}

void test_market_order_no_liquidity_does_not_rest() {
    OrderBook book("AAPL");
    auto res = book.submit(make_order(1, Side::Buy, OrderType::Market, 0.0, 10));
    CHECK(res.fills.empty());
    auto snap = book.snapshot();
    CHECK(snap.bids.empty()); // market orders never rest
}

void test_cancel_resting_order() {
    OrderBook book("AAPL");
    book.submit(make_order(1, Side::Buy, OrderType::Limit, 100.0, 10));
    bool ok = book.cancel(1);
    CHECK(ok);
    auto snap = book.snapshot();
    CHECK(snap.bids.empty());
    CHECK(!book.cancel(1)); // second cancel should fail (already gone)
}

void test_zero_qty_rejected() {
    OrderBook book("AAPL");
    auto res = book.submit(make_order(1, Side::Buy, OrderType::Limit, 100.0, 0));
    CHECK(!res.accepted);
}

void test_crossed_market_multi_level_partial() {
    OrderBook book("AAPL");
    book.submit(make_order(1, Side::Sell, OrderType::Limit, 100.0, 3));
    book.submit(make_order(2, Side::Sell, OrderType::Limit, 100.0, 4)); // same level, FIFO after #1
    auto res = book.submit(make_order(3, Side::Buy, OrderType::Limit, 100.0, 5));
    CHECK(res.fills.size() == 2);
    CHECK(res.fills[0].resting_order_id == 1);
    CHECK(res.fills[0].qty == 3);
    CHECK(res.fills[1].resting_order_id == 2);
    CHECK(res.fills[1].qty == 2);
    auto snap = book.snapshot();
    CHECK(snap.asks.size() == 1);
    CHECK(snap.asks[0].qty == 2); // remainder of order 2
}

int main() {
    test_resting_order_no_match();
    test_simple_full_match();
    test_partial_match_leaves_remainder();
    test_price_priority();
    test_time_priority_fifo_same_price();
    test_market_order_sweeps_book();
    test_market_order_no_liquidity_does_not_rest();
    test_cancel_resting_order();
    test_zero_qty_rejected();
    test_crossed_market_multi_level_partial();

    if (g_failures == 0) {
        std::cout << "All order book tests passed.\n";
        return 0;
    }
    std::cerr << g_failures << " test(s) failed.\n";
    return 1;
}
