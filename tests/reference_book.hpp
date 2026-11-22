#pragma once

// A deliberately naive order book: one flat vector, linear scans everywhere.
// It shares no code with OrderBook, so randomised differential testing against
// it catches bookkeeping bugs in the fast implementation.

#include <algorithm>
#include <cstdint>
#include <map>
#include <optional>
#include <vector>

#include "lob/order_book.hpp"

namespace lob::testing {

class ReferenceBook {
public:
    Result submit_limit(OrderId id, Side side, Price price, Qty qty, TimeInForce tif = TimeInForce::GTC) {
        return execute(id, side, price, qty, tif);
    }
    Result submit_market(OrderId id, Side side, Qty qty) { return execute(id, side, std::nullopt, qty, TimeInForce::IOC); }

    bool cancel(OrderId id) {
        auto it = find(id);
        if (it == orders_.end()) return false;
        orders_.erase(it);
        return true;
    }

    bool reduce(OrderId id, Qty new_qty) {
        auto it = find(id);
        if (it == orders_.end() || new_qty <= 0 || new_qty >= it->remaining) return false;
        it->remaining = new_qty;
        return true;
    }

    Result replace(OrderId id, Price new_price, Qty new_qty) {
        auto it = find(id);
        if (it == orders_.end() || new_qty <= 0) return Result{};
        const Side side = it->side;
        orders_.erase(it);
        return submit_limit(id, side, new_price, new_qty);
    }

    std::vector<Level> depth(Side side) const {
        std::map<Price, Level> levels;
        for (const auto& o : orders_) {
            if (o.side != side) continue;
            auto& level = levels.try_emplace(o.price, Level{o.price, 0, 0}).first->second;
            level.qty += o.remaining;
            level.orders += 1;
        }
        std::vector<Level> out;
        for (const auto& [price, level] : levels) out.push_back(level);
        if (side == Side::Buy) std::reverse(out.begin(), out.end());
        return out;
    }

private:
    struct Order {
        OrderId id;
        Side side;
        Price price;
        Qty remaining;
        std::uint64_t seq;
    };

    std::vector<Order>::iterator find(OrderId id) {
        return std::find_if(orders_.begin(), orders_.end(), [&](const Order& o) { return o.id == id; });
    }

    static bool acceptable(Side aggressor, Price resting, std::optional<Price> limit) {
        if (!limit) return true;
        return aggressor == Side::Buy ? resting <= *limit : resting >= *limit;
    }

    // Best opposite order: best price, then earliest arrival.
    std::vector<Order>::iterator best_opposite(Side aggressor, std::optional<Price> limit) {
        auto best = orders_.end();
        for (auto it = orders_.begin(); it != orders_.end(); ++it) {
            if (it->side == aggressor || !acceptable(aggressor, it->price, limit)) continue;
            if (best == orders_.end()) {
                best = it;
                continue;
            }
            const bool better_price = aggressor == Side::Buy ? it->price < best->price : it->price > best->price;
            if (better_price || (it->price == best->price && it->seq < best->seq)) best = it;
        }
        return best;
    }

    Result execute(OrderId id, Side side, std::optional<Price> limit, Qty qty, TimeInForce tif) {
        Result result;
        if (qty <= 0 || find(id) != orders_.end()) return result;
        result.accepted = true;
        if (tif == TimeInForce::FOK) {
            Qty available = 0;
            for (const auto& o : orders_)
                if (o.side != side && acceptable(side, o.price, limit)) available += o.remaining;
            if (available < qty) return result;
        }
        while (result.filled < qty) {
            auto best = best_opposite(side, limit);
            if (best == orders_.end()) break;
            const Qty take = std::min(qty - result.filled, best->remaining);
            result.trades.push_back(Trade{best->id, id, side, best->price, take});
            result.filled += take;
            best->remaining -= take;
            if (best->remaining == 0) orders_.erase(best);
        }
        const Qty left = qty - result.filled;
        if (left > 0 && limit && tif == TimeInForce::GTC) {
            orders_.push_back(Order{id, side, *limit, left, next_seq_++});
            result.resting = left;
        }
        return result;
    }

    std::vector<Order> orders_;
    std::uint64_t next_seq_ = 0;
};

}  // namespace lob::testing
