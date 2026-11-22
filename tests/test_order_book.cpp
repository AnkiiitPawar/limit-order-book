#include <cstdio>
#include <functional>
#include <random>
#include <string>
#include <vector>

#include "lob/order_book.hpp"
#include "reference_book.hpp"

// ---- minimal test harness -------------------------------------------------
namespace {
int g_failures = 0;
std::vector<std::pair<std::string, std::function<void()>>>& registry() {
    static std::vector<std::pair<std::string, std::function<void()>>> tests;
    return tests;
}
struct Register {
    Register(const char* name, std::function<void()> fn) { registry().emplace_back(name, std::move(fn)); }
};
}  // namespace

#define TEST(name)                                  \
    static void name();                             \
    static const Register register_##name(#name, name); \
    static void name()

// Variadic so that brace-initialisers containing commas can appear in a check.
#define CHECK(...)                                                                               \
    do {                                                                                         \
        if (!(__VA_ARGS__)) {                                                                    \
            std::fprintf(stderr, "  %s:%d: CHECK failed: %s\n", __FILE__, __LINE__, #__VA_ARGS__); \
            ++g_failures;                                                                        \
        }                                                                                        \
    } while (0)

using namespace lob;

// ---- behaviour --------------------------------------------------------------

TEST(non_crossing_orders_rest_without_trading) {
    OrderBook book;
    auto a = book.submit_limit(1, Side::Buy, 100, 10);
    auto b = book.submit_limit(2, Side::Sell, 101, 5);
    CHECK(a.accepted && a.trades.empty() && a.resting == 10);
    CHECK(b.accepted && b.trades.empty() && b.resting == 5);
    CHECK(book.best_bid() == 100 && book.best_ask() == 101);
}

TEST(crossing_order_trades_at_the_resting_price) {
    OrderBook book;
    book.submit_limit(1, Side::Sell, 100, 10);
    auto r = book.submit_limit(2, Side::Buy, 105, 4);  // willing to pay 105, gets 100
    CHECK(r.filled == 4 && r.resting == 0);
    CHECK(r.trades.size() == 1 && r.trades[0].price == 100 && r.trades[0].resting_id == 1);
    CHECK(book.depth(Side::Sell) == std::vector<Level>{{100, 6, 1}});
}

TEST(time_priority_within_a_level) {
    OrderBook book;
    book.submit_limit(1, Side::Sell, 100, 5);
    book.submit_limit(2, Side::Sell, 100, 5);
    book.submit_limit(3, Side::Sell, 100, 5);
    auto r = book.submit_market(9, Side::Buy, 7);
    CHECK(r.trades.size() == 2);
    CHECK(r.trades[0].resting_id == 1 && r.trades[0].qty == 5);
    CHECK(r.trades[1].resting_id == 2 && r.trades[1].qty == 2);
}

TEST(price_priority_sweeps_best_levels_first) {
    OrderBook book;
    book.submit_limit(1, Side::Buy, 99, 5);
    book.submit_limit(2, Side::Buy, 101, 5);
    book.submit_limit(3, Side::Buy, 100, 5);
    auto r = book.submit_limit(9, Side::Sell, 100, 12);
    CHECK(r.trades.size() == 2);
    CHECK(r.trades[0].price == 101 && r.trades[1].price == 100);
    CHECK(r.filled == 10 && r.resting == 2);
    CHECK(book.best_ask() == 100 && book.best_bid() == 99);
}

TEST(market_order_sweeps_and_never_rests) {
    OrderBook book;
    book.submit_limit(1, Side::Sell, 100, 3);
    book.submit_limit(2, Side::Sell, 102, 3);
    auto r = book.submit_market(9, Side::Buy, 10);
    CHECK(r.filled == 6 && r.resting == 0 && !book.contains(9));
    CHECK(!book.best_ask().has_value());
}

TEST(ioc_cancels_its_remainder) {
    OrderBook book;
    book.submit_limit(1, Side::Sell, 100, 3);
    auto r = book.submit_limit(9, Side::Buy, 100, 10, TimeInForce::IOC);
    CHECK(r.filled == 3 && r.resting == 0 && !book.contains(9));
}

TEST(fok_is_all_or_nothing) {
    OrderBook book;
    book.submit_limit(1, Side::Sell, 100, 3);
    book.submit_limit(2, Side::Sell, 101, 3);
    auto killed = book.submit_limit(8, Side::Buy, 101, 7, TimeInForce::FOK);
    CHECK(killed.accepted && killed.filled == 0 && killed.trades.empty());
    CHECK(book.order_count() == 2);
    auto filled = book.submit_limit(9, Side::Buy, 101, 6, TimeInForce::FOK);
    CHECK(filled.filled == 6 && book.order_count() == 0);
}

TEST(cancel_removes_the_order_and_empty_levels) {
    OrderBook book;
    book.submit_limit(1, Side::Buy, 100, 5);
    book.submit_limit(2, Side::Buy, 100, 7);
    CHECK(book.cancel(1));
    CHECK(!book.cancel(1));
    CHECK(book.depth(Side::Buy) == std::vector<Level>{{100, 7, 1}});
    CHECK(book.cancel(2));
    CHECK(!book.best_bid().has_value());
}

TEST(reduce_keeps_priority_but_replace_loses_it) {
    OrderBook book;
    book.submit_limit(1, Side::Sell, 100, 10);
    book.submit_limit(2, Side::Sell, 100, 10);
    CHECK(book.reduce(1, 4));
    CHECK(!book.reduce(1, 4));   // not a reduction
    CHECK(!book.reduce(1, 20));  // growing is not allowed in place
    auto r1 = book.submit_market(8, Side::Buy, 1);
    CHECK(r1.trades[0].resting_id == 1);  // still first in the queue

    book.replace(1, 100, 3);  // same price, but now behind order 2
    auto r2 = book.submit_market(9, Side::Buy, 1);
    CHECK(r2.trades[0].resting_id == 2);
}

TEST(invalid_orders_are_rejected) {
    OrderBook book;
    CHECK(!book.submit_limit(1, Side::Buy, 100, 0).accepted);
    CHECK(book.submit_limit(1, Side::Buy, 100, 5).accepted);
    CHECK(!book.submit_limit(1, Side::Sell, 200, 5).accepted);  // id 1 is live
    CHECK(!book.replace(42, 100, 5).accepted);                  // unknown id
}

// ---- randomised differential test ------------------------------------------

TEST(matches_a_naive_reference_book_on_random_order_flow) {
    OrderBook fast;
    testing::ReferenceBook slow;
    std::mt19937_64 rng(20261122);
    std::uniform_int_distribution<int> kind(0, 99), offset(-6, 6), size(1, 20), side_pick(0, 1), tif_pick(0, 9);
    std::vector<OrderId> ids;
    OrderId next_id = 1;
    const Price mid = 1000;

    for (int step = 0; step < 30000; ++step) {
        const Side side = side_pick(rng) ? Side::Buy : Side::Sell;
        const int k = kind(rng);
        Result a, b;
        bool ca = false, cb = false;
        if (k < 60) {
            const TimeInForce tif = tif_pick(rng) == 0 ? TimeInForce::IOC : tif_pick(rng) == 0 ? TimeInForce::FOK : TimeInForce::GTC;
            // occasionally reuse an id to exercise duplicate rejection
            const OrderId id = (!ids.empty() && kind(rng) < 3) ? ids[rng() % ids.size()] : next_id++;
            const Price price = mid + offset(rng);
            const Qty qty = size(rng);
            a = fast.submit_limit(id, side, price, qty, tif);
            b = slow.submit_limit(id, side, price, qty, tif);
            ids.push_back(id);
        } else if (k < 85 && !ids.empty()) {
            const OrderId id = ids[rng() % ids.size()];
            ca = fast.cancel(id);
            cb = slow.cancel(id);
        } else if (k < 92 && !ids.empty()) {
            const OrderId id = ids[rng() % ids.size()];
            const Qty q = size(rng);
            ca = fast.reduce(id, q);
            cb = slow.reduce(id, q);
        } else if (k < 96 && !ids.empty()) {
            const OrderId id = ids[rng() % ids.size()];
            const Price p = mid + offset(rng);
            const Qty q = size(rng);
            a = fast.replace(id, p, q);
            b = slow.replace(id, p, q);
        } else {
            const OrderId id = next_id++;
            const Qty q = size(rng) * 3;
            a = fast.submit_market(id, side, q);
            b = slow.submit_market(id, side, q);
        }
        CHECK(a == b);
        CHECK(ca == cb);
        CHECK(fast.depth(Side::Buy) == slow.depth(Side::Buy));
        CHECK(fast.depth(Side::Sell) == slow.depth(Side::Sell));
        const auto bid = fast.best_bid(), ask = fast.best_ask();
        CHECK(!bid || !ask || *bid < *ask);  // the book is never left crossed
        if (g_failures > 0) {
            std::fprintf(stderr, "  diverged at step %d\n", step);
            return;
        }
    }
}

int main() {
    for (const auto& [name, fn] : registry()) {
        const int before = g_failures;
        fn();
        std::printf("%s %s\n", g_failures == before ? "PASS" : "FAIL", name.c_str());
    }
    std::printf("%zu tests, %d failed checks\n", registry().size(), g_failures);
    return g_failures == 0 ? 0 : 1;
}
