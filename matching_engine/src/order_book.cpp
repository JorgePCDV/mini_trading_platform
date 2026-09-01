#include "order_book.hpp"

namespace engine {

static uint64_t g_exec_seq = 1;

SubmitResult OrderBook::submit(Order order) {
    SubmitResult result;
    result.remaining_qty = order.qty;

    if (order.qty == 0) {
        result.accepted = false;
        result.reject_reason = "quantity must be > 0";
        return result;
    }
    if (order.type == OrderType::Limit && order.price <= 0.0) {
        result.accepted = false;
        result.reject_reason = "limit price must be > 0";
        return result;
    }

    if (order.side == Side::Buy) {
        // Match against asks (lowest price first) while price crosses.
        auto it = asks_.begin();
        while (order.qty > 0 && it != asks_.end()) {
            bool crosses = (order.type == OrderType::Market) || (order.price >= it->first);
            if (!crosses) break;

            auto& level = it->second;
            while (order.qty > 0 && !level.empty()) {
                Order& resting = level.front();
                uint64_t traded_qty = std::min(order.qty, resting.qty);
                double traded_price = resting.price; // resting order sets the price

                Fill f;
                f.exec_id = g_exec_seq++;
                f.symbol = symbol_;
                f.aggressor_order_id = order.order_id;
                f.aggressor_client_id = order.client_id;
                f.aggressor_client_order_id = order.client_order_id;
                f.resting_order_id = resting.order_id;
                f.resting_client_id = resting.client_id;
                f.resting_client_order_id = resting.client_order_id;
                f.aggressor_side = order.side;
                f.price = traded_price;
                f.qty = traded_qty;
                f.ts_ns = now_nanos();
                result.fills.push_back(f);
                last_trade_price_ = traded_price;

                order.qty -= traded_qty;
                resting.qty -= traded_qty;

                if (resting.qty == 0) {
                    locations_.erase(resting.order_id);
                    level.pop_front();
                }
            }
            if (level.empty()) {
                it = asks_.erase(it);
            } else {
                break; // level partially consumed only if incoming exhausted
            }
        }

        if (order.qty > 0 && order.type == OrderType::Limit) {
            // rest remaining quantity on the book
            order.original_qty = order.qty;
            bids_[order.price].push_back(order);
            locations_[order.order_id] = Location{Side::Buy, order.price};
        }
    } else { // Sell
        auto it = bids_.begin();
        while (order.qty > 0 && it != bids_.end()) {
            bool crosses = (order.type == OrderType::Market) || (order.price <= it->first);
            if (!crosses) break;

            auto& level = it->second;
            while (order.qty > 0 && !level.empty()) {
                Order& resting = level.front();
                uint64_t traded_qty = std::min(order.qty, resting.qty);
                double traded_price = resting.price;

                Fill f;
                f.exec_id = g_exec_seq++;
                f.symbol = symbol_;
                f.aggressor_order_id = order.order_id;
                f.aggressor_client_id = order.client_id;
                f.aggressor_client_order_id = order.client_order_id;
                f.resting_order_id = resting.order_id;
                f.resting_client_id = resting.client_id;
                f.resting_client_order_id = resting.client_order_id;
                f.aggressor_side = order.side;
                f.price = traded_price;
                f.qty = traded_qty;
                f.ts_ns = now_nanos();
                result.fills.push_back(f);
                last_trade_price_ = traded_price;

                order.qty -= traded_qty;
                resting.qty -= traded_qty;

                if (resting.qty == 0) {
                    locations_.erase(resting.order_id);
                    level.pop_front();
                }
            }
            if (level.empty()) {
                it = bids_.erase(it);
            } else {
                break;
            }
        }

        if (order.qty > 0 && order.type == OrderType::Limit) {
            order.original_qty = order.qty;
            asks_[order.price].push_back(order);
            locations_[order.order_id] = Location{Side::Sell, order.price};
        }
    }

    result.remaining_qty = order.qty;
    return result;
}

bool OrderBook::cancel(uint64_t order_id, std::string* out_symbol) {
    auto loc_it = locations_.find(order_id);
    if (loc_it == locations_.end()) return false;
    Location loc = loc_it->second;

    auto erase_from = [&](auto& book_side) -> bool {
        auto level_it = book_side.find(loc.price);
        if (level_it == book_side.end()) return false;
        auto& dq = level_it->second;
        for (auto o_it = dq.begin(); o_it != dq.end(); ++o_it) {
            if (o_it->order_id == order_id) {
                dq.erase(o_it);
                if (dq.empty()) book_side.erase(level_it);
                return true;
            }
        }
        return false;
    };

    bool ok = (loc.side == Side::Buy) ? erase_from(bids_) : erase_from(asks_);
    if (ok) {
        locations_.erase(loc_it);
        if (out_symbol) *out_symbol = symbol_;
    }
    return ok;
}

BookSnapshot OrderBook::snapshot(size_t depth) const {
    BookSnapshot snap;
    snap.symbol = symbol_;
    size_t count = 0;
    for (const auto& [price, dq] : bids_) {
        if (count++ >= depth) break;
        uint64_t total = 0;
        for (const auto& o : dq) total += o.qty;
        snap.bids.push_back({price, total});
    }
    count = 0;
    for (const auto& [price, dq] : asks_) {
        if (count++ >= depth) break;
        uint64_t total = 0;
        for (const auto& o : dq) total += o.qty;
        snap.asks.push_back({price, total});
    }
    return snap;
}

} // namespace engine
