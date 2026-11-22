// Replays synthetic order flow and reports throughput and per-operation latency.
// usage: lob_bench [n_ops]

#include <algorithm>
#include <chrono>
#include <cstdint>
#include <cstdio>
#include <random>
#include <string>
#include <vector>

#include "lob/order_book.hpp"

using namespace lob;
using Clock = std::chrono::steady_clock;

namespace {

struct Op {
    enum Kind : std::uint8_t { Limit, Cancel, Market } kind;
    Side side;
    OrderId id;
    Price price;
    Qty qty;
};

// 70% limit orders within 20 ticks of a drifting mid, 25% cancels of recent
// orders, 5% market orders. Generated up front so the RNG is not timed.
std::vector<Op> make_flow(std::size_t n, std::uint64_t seed) {
    std::mt19937_64 rng(seed);
    std::uniform_int_distribution<int> pct(0, 99), away(0, 20), size(1, 100), coin(0, 1), drift(-1, 1);
    std::vector<Op> ops;
    ops.reserve(n);
    std::vector<OrderId> recent;
    Price mid = 100'000;
    OrderId next = 1;
    for (std::size_t i = 0; i < n; ++i) {
        if (i % 64 == 0) mid += drift(rng);
        const int p = pct(rng);
        const Side side = coin(rng) ? Side::Buy : Side::Sell;
        if (p < 70) {
            // mostly passive, sometimes 1-2 ticks through the mid
            const Price px = side == Side::Buy ? mid - away(rng) + 2 : mid + away(rng) - 2;
            ops.push_back({Op::Limit, side, next, px, size(rng)});
            recent.push_back(next++);
            if (recent.size() > 4096) recent.erase(recent.begin(), recent.begin() + 2048);
        } else if (p < 95 && !recent.empty()) {
            ops.push_back({Op::Cancel, side, recent[rng() % recent.size()], 0, 0});
        } else {
            ops.push_back({Op::Market, side, next++, 0, size(rng)});
        }
    }
    return ops;
}

inline void apply(OrderBook& book, const Op& op) {
    switch (op.kind) {
        case Op::Limit: book.submit_limit(op.id, op.side, op.price, op.qty); break;
        case Op::Cancel: book.cancel(op.id); break;
        case Op::Market: book.submit_market(op.id, op.side, op.qty); break;
    }
}

}  // namespace

int main(int argc, char** argv) {
    const std::size_t n = argc > 1 ? std::stoull(argv[1]) : 2'000'000;
    const auto ops = make_flow(n, 42);

    // Throughput: no per-op timing in the loop.
    {
        OrderBook book;
        const auto t0 = Clock::now();
        for (const auto& op : ops) apply(book, op);
        const double secs = std::chrono::duration<double>(Clock::now() - t0).count();
        std::printf("throughput : %.2f M ops/s  (%zu ops in %.3f s, %zu orders resting at end)\n",
                    n / secs / 1e6, n, secs, book.order_count());
    }

    // Latency distribution: time every operation (includes ~20 ns of clock overhead).
    {
        OrderBook book;
        std::vector<std::uint32_t> ns(n);
        for (std::size_t i = 0; i < n; ++i) {
            const auto t0 = Clock::now();
            apply(book, ops[i]);
            ns[i] = static_cast<std::uint32_t>(std::chrono::duration_cast<std::chrono::nanoseconds>(Clock::now() - t0).count());
        }
        std::sort(ns.begin(), ns.end());
        auto q = [&](double f) { return ns[static_cast<std::size_t>(f * (n - 1))]; };
        std::printf("latency ns : p50 %u | p90 %u | p99 %u | p99.9 %u | max %u\n", q(0.50), q(0.90), q(0.99), q(0.999), ns.back());
    }
    return 0;
}
