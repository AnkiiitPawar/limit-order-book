#pragma once

#include <cstddef>
#include <cstdint>
#include <functional>
#include <list>
#include <map>
#include <optional>
#include <unordered_map>
#include <vector>

namespace lob {

using OrderId = std::uint64_t;
using Price = std::int64_t;  // integer ticks: no floating-point price comparisons
using Qty = std::int64_t;

enum class Side : std::uint8_t { Buy, Sell };

// GTC rests any unfilled remainder, IOC cancels it, FOK trades in full or not at all.
enum class TimeInForce : std::uint8_t { GTC, IOC, FOK };

struct Trade {
    OrderId resting_id;
    OrderId aggressor_id;
    Side aggressor_side;
    Price price;  // always the resting order's price
    Qty qty;
    friend bool operator==(const Trade&, const Trade&) = default;
};

struct Result {
    bool accepted = false;  // false: non-positive quantity or an id that is already live
    Qty filled = 0;         // traded immediately
    Qty resting = 0;        // left on the book (GTC limit orders only)
    std::vector<Trade> trades;
    friend bool operator==(const Result&, const Result&) = default;
};

struct Level {
    Price price;
    Qty qty;
    std::size_t orders;
    friend bool operator==(const Level&, const Level&) = default;
};

// Price-time priority limit order book for one instrument.
//
// Each side is a sorted map from price to a FIFO queue of orders, plus a hash
// map from order id to the order's position in its queue. That gives
//   best price          O(1)
//   add at a new level  O(log L)   (L = number of price levels)
//   cancel / reduce     O(1) + O(log L) to find the level
//   each fill           O(1)
class OrderBook {
public:
    Result submit_limit(OrderId id, Side side, Price price, Qty qty, TimeInForce tif = TimeInForce::GTC);
    Result submit_market(OrderId id, Side side, Qty qty);  // never rests

    bool cancel(OrderId id);
    // Lower a resting order's quantity in place; it keeps its place in the queue.
    bool reduce(OrderId id, Qty new_qty);
    // Cancel and re-enter at a new price/quantity; it goes to the back of the queue.
    Result replace(OrderId id, Price new_price, Qty new_qty);

    std::optional<Price> best_bid() const;
    std::optional<Price> best_ask() const;
    std::vector<Level> depth(Side side, std::size_t max_levels = SIZE_MAX) const;
    std::size_t order_count() const { return index_.size(); }
    bool contains(OrderId id) const { return index_.count(id) != 0; }

private:
    struct Resting {
        OrderId id;
        Qty remaining;
    };
    struct Queue {
        std::list<Resting> orders;
        Qty total = 0;
    };
    using BidLevels = std::map<Price, Queue, std::greater<Price>>;  // best (highest) first
    using AskLevels = std::map<Price, Queue, std::less<Price>>;     // best (lowest) first
    struct Locator {
        Side side;
        Price price;
        std::list<Resting>::iterator it;
    };

    Result execute(OrderId id, Side side, std::optional<Price> limit, Qty qty, TimeInForce tif);
    void rest(OrderId id, Side side, Price price, Qty qty);

    template <class Levels>
    Qty match(Levels& levels, OrderId aggressor, Side side, std::optional<Price> limit, Qty qty,
              std::vector<Trade>& trades);
    template <class Levels>
    static Qty liquidity(const Levels& levels, Side aggressor, std::optional<Price> limit, Qty needed);

    BidLevels bids_;
    AskLevels asks_;
    std::unordered_map<OrderId, Locator> index_;
};

}  // namespace lob
