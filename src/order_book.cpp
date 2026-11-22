#include "lob/order_book.hpp"

#include <algorithm>
#include <iterator>

namespace lob {
namespace {

// Can an aggressor with this limit trade against a resting order at `resting`?
bool acceptable(Side aggressor, Price resting, std::optional<Price> limit) {
    if (!limit) return true;  // market order
    return aggressor == Side::Buy ? resting <= *limit : resting >= *limit;
}

}  // namespace

template <class Levels>
Qty OrderBook::liquidity(const Levels& levels, Side aggressor, std::optional<Price> limit, Qty needed) {
    Qty total = 0;
    for (const auto& [price, queue] : levels) {
        if (total >= needed || !acceptable(aggressor, price, limit)) break;
        total += queue.total;
    }
    return total;
}

template <class Levels>
Qty OrderBook::match(Levels& levels, OrderId aggressor, Side side, std::optional<Price> limit, Qty qty,
                     std::vector<Trade>& trades) {
    Qty filled = 0;
    while (filled < qty && !levels.empty()) {
        auto level = levels.begin();
        if (!acceptable(side, level->first, limit)) break;
        Queue& queue = level->second;
        while (filled < qty && !queue.orders.empty()) {
            Resting& resting = queue.orders.front();
            const Qty take = std::min(qty - filled, resting.remaining);
            trades.push_back(Trade{resting.id, aggressor, side, level->first, take});
            resting.remaining -= take;
            queue.total -= take;
            filled += take;
            if (resting.remaining == 0) {
                index_.erase(resting.id);
                queue.orders.pop_front();
            }
        }
        if (queue.orders.empty()) levels.erase(level);
    }
    return filled;
}

void OrderBook::rest(OrderId id, Side side, Price price, Qty qty) {
    auto add = [&](auto& levels) {
        Queue& queue = levels[price];
        queue.orders.push_back(Resting{id, qty});
        queue.total += qty;
        index_.emplace(id, Locator{side, price, std::prev(queue.orders.end())});
    };
    if (side == Side::Buy) {
        add(bids_);
    } else {
        add(asks_);
    }
}

Result OrderBook::execute(OrderId id, Side side, std::optional<Price> limit, Qty qty, TimeInForce tif) {
    Result result;
    if (qty <= 0 || contains(id)) return result;
    result.accepted = true;

    if (tif == TimeInForce::FOK) {
        const Qty available =
            side == Side::Buy ? liquidity(asks_, side, limit, qty) : liquidity(bids_, side, limit, qty);
        if (available < qty) return result;  // killed: nothing trades
    }

    result.filled = side == Side::Buy ? match(asks_, id, side, limit, qty, result.trades)
                                      : match(bids_, id, side, limit, qty, result.trades);
    const Qty left = qty - result.filled;
    if (left > 0 && limit && tif == TimeInForce::GTC) {
        rest(id, side, *limit, left);
        result.resting = left;
    }
    return result;
}

Result OrderBook::submit_limit(OrderId id, Side side, Price price, Qty qty, TimeInForce tif) {
    return execute(id, side, price, qty, tif);
}

Result OrderBook::submit_market(OrderId id, Side side, Qty qty) {
    return execute(id, side, std::nullopt, qty, TimeInForce::IOC);
}

bool OrderBook::cancel(OrderId id) {
    auto found = index_.find(id);
    if (found == index_.end()) return false;
    const Locator loc = found->second;
    auto remove = [&](auto& levels) {
        auto level = levels.find(loc.price);
        level->second.total -= loc.it->remaining;
        level->second.orders.erase(loc.it);
        if (level->second.orders.empty()) levels.erase(level);
    };
    if (loc.side == Side::Buy) {
        remove(bids_);
    } else {
        remove(asks_);
    }
    index_.erase(found);
    return true;
}

bool OrderBook::reduce(OrderId id, Qty new_qty) {
    auto found = index_.find(id);
    if (found == index_.end() || new_qty <= 0) return false;  // use cancel() to remove
    Locator& loc = found->second;
    if (new_qty >= loc.it->remaining) return false;  // growing an order must lose priority: replace()
    const Qty delta = loc.it->remaining - new_qty;
    loc.it->remaining = new_qty;
    Qty& total = loc.side == Side::Buy ? bids_.find(loc.price)->second.total : asks_.find(loc.price)->second.total;
    total -= delta;
    return true;
}

Result OrderBook::replace(OrderId id, Price new_price, Qty new_qty) {
    auto found = index_.find(id);
    if (found == index_.end() || new_qty <= 0) return Result{};
    const Side side = found->second.side;
    cancel(id);
    return submit_limit(id, side, new_price, new_qty);
}

std::optional<Price> OrderBook::best_bid() const {
    if (bids_.empty()) return std::nullopt;
    return bids_.begin()->first;
}

std::optional<Price> OrderBook::best_ask() const {
    if (asks_.empty()) return std::nullopt;
    return asks_.begin()->first;
}

std::vector<Level> OrderBook::depth(Side side, std::size_t max_levels) const {
    std::vector<Level> out;
    auto collect = [&](const auto& levels) {
        for (const auto& [price, queue] : levels) {
            if (out.size() == max_levels) break;
            out.push_back(Level{price, queue.total, queue.orders.size()});
        }
    };
    if (side == Side::Buy) {
        collect(bids_);
    } else {
        collect(asks_);
    }
    return out;
}

}  // namespace lob
