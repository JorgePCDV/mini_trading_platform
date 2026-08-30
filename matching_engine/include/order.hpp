#pragma once
#include <string>
#include <cstdint>
#include <chrono>

namespace engine {

    enum class Side { Buy, Sell };
    enum class OrderType { Limit, Market };
    enum class TimeInForce { GTC, IOC, FOK };

    inline Side side_from_string(const std::string& s) {
        return (s == "buy" || s == "BUY") ? Side::Buy : Side::Sell;
    }
    inline std::string side_to_string(Side s) { return s == Side::Buy ? "buy" : "sell"; }

    inline OrderType type_from_string(const std::string& s) {
        return (s == "market" || s == "MARKET") ? OrderType::Market : OrderType::Limit;
    }

    inline int64_t now_nanos() {
        return std::chrono::duration_cast<std::chrono::nanoseconds>(
                   std::chrono::system_clock::now().time_since_epoch())
            .count();
    }

    // A resting/incoming order in the book.
    struct Order {
        uint64_t order_id{0};       // engine-assigned sequence id (used for time priority)
        std::string client_order_id; // id the client sent us, echoed back
        std::string client_id;
        std::string symbol;
        Side side{Side::Buy};
        OrderType type{OrderType::Limit};
        double price{0.0};          // ignored for market orders
        uint64_t qty{0};            // remaining quantity
        uint64_t original_qty{0};
        int64_t ts_ns{0};           // arrival timestamp, used for FIFO priority at same price
    };

    // A single execution resulting from a match.
    struct Fill {
        uint64_t exec_id{0};
        std::string symbol;
        uint64_t aggressor_order_id{0};
        std::string aggressor_client_id;
        std::string aggressor_client_order_id;
        uint64_t resting_order_id{0};
        std::string resting_client_id;
        std::string resting_client_order_id;
        Side aggressor_side{Side::Buy};
        double price{0.0};
        uint64_t qty{0};
        int64_t ts_ns{0};
    };

} // namespace engine
