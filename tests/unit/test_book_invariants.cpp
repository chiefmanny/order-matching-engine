//
//  test_book_invariants.cpp
//
//  Fast, single-purpose tests for the order-book bookkeeping and trade-tape
//  guarantees the experiment harness depends on:
//
//    * LimitPrice::totalSize (what GetLevel2 reports as displayed depth) always
//      equals the sum of the resting order sizes at that level.
//    * A trade prints at the MAKER's limit price, and reports the correct
//      aggressor side and maker/taker order ids.
//    * The trade tape fans out: every subscriber sees every fill.
//
//  These use the real MatchingAlgorithm against a real OrderBook, but no gRPC.
//

#include "order_book.h"
#include "matching_algorithm.h"

#include <atomic>
#include <chrono>
#include <cmath>
#include <iostream>
#include <shared_mutex>
#include <string>
#include <thread>
#include <vector>

namespace {

int failures = 0;

void check(bool cond, const std::string& what) {
    if (cond) {
        std::cout << "  [ok] " << what << std::endl;
    } else {
        std::cout << "  [FAIL] " << what << std::endl;
        ++failures;
    }
}

template <typename T>
void checkEq(const T& actual, const T& expected, const std::string& what) {
    if (actual == expected) {
        std::cout << "  [ok] " << what << std::endl;
    } else {
        std::cout << "  [FAIL] " << what << " (expected " << expected
                  << ", got " << actual << ")" << std::endl;
        ++failures;
    }
}

void checkNear(double actual, double expected, const std::string& what) {
    if (std::fabs(actual - expected) < 1e-9) {
        std::cout << "  [ok] " << what << std::endl;
    } else {
        std::cout << "  [FAIL] " << what << " (expected " << expected
                  << ", got " << actual << ")" << std::endl;
        ++failures;
    }
}

// Holds a book plus its matching thread, and lets a test drive matching to
// quiescence deterministically enough for assertions.
struct Harness {
    std::shared_mutex askMutex;
    std::shared_mutex bidMutex;
    OrderBook book;
    MatchingAlgorithm algo;
    std::atomic<bool> running{true};
    std::thread thread;

    Harness() : book("TEST", 1, askMutex, bidMutex), algo(book, askMutex, bidMutex) {}

    void start() {
        thread = std::thread([this] { algo.run(running, false, false); });
    }

    void stop() {
        running = false;
        if (thread.joinable()) thread.join();
    }

    // The matching loop polls every 1ms; give it room to drain, then settle.
    void settle(int ms = 120) {
        std::this_thread::sleep_for(std::chrono::milliseconds(ms));
    }

    ~Harness() { stop(); }
};

// Sum of LimitPrice::totalSize across a side -- i.e. what the Level 2 feed shows.
unsigned int displayedDepth(OrderBook& book, bool buy) {
    unsigned int sum = 0;
    auto levels = buy ? book.getTopBidsWithLock(1000) : book.getTopAsksWithLock(1000);
    for (const auto& level : levels) sum += level.second;
    return sum;
}

// ---------------------------------------------------------------------------

void test_depth_matches_resting_orders_on_add() {
    std::cout << "test_depth_matches_resting_orders_on_add" << std::endl;
    Harness h;

    h.book.addLimitOrder(100.0, 500, true);
    h.book.addLimitOrder(100.0, 300, true);   // same level, two orders
    h.book.addLimitOrder(99.0,  200, true);
    h.book.addLimitOrder(101.0, 400, false);

    checkEq(displayedDepth(h.book, true), h.book.totalRestingSize(true),
            "bid displayed depth == sum of resting bid orders");
    checkEq(displayedDepth(h.book, true), 1000u, "bid depth is 500+300+200");
    checkEq(displayedDepth(h.book, false), 400u, "ask depth is 400");

    auto bids = h.book.getTopBidsWithLock(10);
    checkEq(bids.at(0).second, 800u, "top bid level aggregates both orders");
}

void test_depth_after_cancel() {
    std::cout << "test_depth_after_cancel" << std::endl;
    Harness h;

    unsigned int a = h.book.addLimitOrder(100.0, 500, true);
    h.book.addLimitOrder(100.0, 300, true);

    check(h.book.cancelLimitOrder(a), "cancel of a live order succeeds");
    checkEq(displayedDepth(h.book, true), 300u, "depth drops by the canceled size");
    checkEq(displayedDepth(h.book, true), h.book.totalRestingSize(true),
            "bid displayed depth == sum of resting bid orders after cancel");

    check(!h.book.cancelLimitOrder(a), "re-cancelling the same id fails");
    checkEq(displayedDepth(h.book, true), 300u, "depth unchanged by the failed re-cancel");
    check(!h.book.cancelLimitOrder(999999999u), "cancelling an unknown id fails");
}

void test_depth_after_partial_and_full_fill() {
    std::cout << "test_depth_after_partial_and_full_fill" << std::endl;
    Harness h;
    h.start();

    // Resting ask of 1000 at 101, then a buy of 400 crosses it.
    h.book.addLimitOrder(101.0, 1000, false);
    h.settle();
    h.book.addLimitOrder(101.0, 400, true);
    h.settle();

    checkEq(displayedDepth(h.book, false), 600u, "ask depth reduced by the partial fill");
    checkEq(displayedDepth(h.book, false), h.book.totalRestingSize(false),
            "ask displayed depth == sum of resting ask orders after partial fill");
    checkEq(displayedDepth(h.book, true), 0u, "aggressing bid fully consumed");

    // Now consume the rest.
    h.book.addLimitOrder(101.0, 600, true);
    h.settle();

    checkEq(displayedDepth(h.book, false), 0u, "ask side empty after full fill");
    checkEq(h.book.totalRestingSize(false), 0u, "no resting ask orders remain");
    h.stop();
}

void test_cancel_after_fill_does_not_corrupt_depth() {
    std::cout << "test_cancel_after_fill_does_not_corrupt_depth" << std::endl;
    Harness h;
    h.start();

    unsigned int doomed = h.book.addLimitOrder(101.0, 300, false);
    h.book.addLimitOrder(101.0, 700, false);   // survivor at the same level
    h.settle();
    h.book.addLimitOrder(101.0, 300, true);    // consumes `doomed` exactly
    h.settle();

    checkEq(displayedDepth(h.book, false), 700u, "depth reflects only the survivor");

    // The stale id must not decrement the level again.
    check(!h.book.cancelLimitOrder(doomed), "cancelling an already-filled id fails");
    checkEq(displayedDepth(h.book, false), 700u, "depth unchanged after the stale cancel");
    checkEq(displayedDepth(h.book, false), h.book.totalRestingSize(false),
            "ask displayed depth == sum of resting ask orders after stale cancel");
    h.stop();
}

void test_trade_prints_at_maker_price_buy_aggressor() {
    std::cout << "test_trade_prints_at_maker_price_buy_aggressor" << std::endl;
    Harness h;
    auto sub = h.book.subscribeFills();
    h.start();

    // Resting ask at 500.10; an aggressive buy limit at 505.00 crosses it.
    unsigned int makerId = h.book.addLimitOrder(500.10, 100, false);
    h.settle();
    unsigned int takerId = h.book.addLimitOrder(505.00, 100, true);
    h.settle();

    std::vector<OrderBook::FillRecord> fills;
    sub->waitAndDrain(fills, std::chrono::milliseconds(200));

    checkEq(fills.size(), size_t{1}, "exactly one fill published");
    if (!fills.empty()) {
        checkNear(fills[0].price, 500.10, "trade prints at the resting ask, not the midpoint");
        checkEq(fills[0].size, uint32_t{100}, "full 100 shares traded");
        check(fills[0].aggressor_is_buy, "buyer is marked as the aggressor");
        checkEq(fills[0].maker_order_id, makerId, "maker id is the resting ask");
        checkEq(fills[0].taker_order_id, takerId, "taker id is the aggressive bid");
        check(fills[0].ts_ns > 0, "fill carries a timestamp");
    }
    h.book.unsubscribeFills(sub);
    h.stop();
}

void test_trade_prints_at_maker_price_sell_aggressor() {
    std::cout << "test_trade_prints_at_maker_price_sell_aggressor" << std::endl;
    Harness h;
    auto sub = h.book.subscribeFills();
    h.start();

    // Resting bid at 499.90; an aggressive sell limit at 495.00 hits it.
    unsigned int makerId = h.book.addLimitOrder(499.90, 100, true);
    h.settle();
    unsigned int takerId = h.book.addLimitOrder(495.00, 100, false);
    h.settle();

    std::vector<OrderBook::FillRecord> fills;
    sub->waitAndDrain(fills, std::chrono::milliseconds(200));

    checkEq(fills.size(), size_t{1}, "exactly one fill published");
    if (!fills.empty()) {
        checkNear(fills[0].price, 499.90, "trade prints at the resting bid, not the midpoint");
        check(!fills[0].aggressor_is_buy, "seller is marked as the aggressor");
        checkEq(fills[0].maker_order_id, makerId, "maker id is the resting bid");
        checkEq(fills[0].taker_order_id, takerId, "taker id is the aggressive ask");
    }
    h.book.unsubscribeFills(sub);
    h.stop();
}

void test_fill_stream_fans_out_to_every_subscriber() {
    std::cout << "test_fill_stream_fans_out_to_every_subscriber" << std::endl;
    Harness h;
    auto recorder    = h.book.subscribeFills();
    auto participant = h.book.subscribeFills();
    checkEq(h.book.fillSubscriberCount(), size_t{2}, "two subscribers registered");
    h.start();

    h.book.addLimitOrder(101.0, 100, false);
    h.book.addLimitOrder(102.0, 100, false);
    h.settle();
    h.book.addLimitOrder(101.0, 100, true);
    h.settle();
    h.book.addLimitOrder(102.0, 100, true);
    h.settle();

    std::vector<OrderBook::FillRecord> a, b;
    recorder->waitAndDrain(a, std::chrono::milliseconds(200));
    participant->waitAndDrain(b, std::chrono::milliseconds(200));

    checkEq(a.size(), size_t{2}, "recorder saw both trades");
    checkEq(b.size(), size_t{2}, "participant saw both trades");
    if (a.size() == 2 && b.size() == 2) {
        check(a[0].price == b[0].price && a[1].price == b[1].price,
              "both subscribers saw an identical tape");
    }
    checkEq(recorder->dropped, uint64_t{0}, "no records dropped");

    h.book.unsubscribeFills(recorder);
    h.book.unsubscribeFills(participant);
    checkEq(h.book.fillSubscriberCount(), size_t{0}, "subscribers deregistered");
    h.stop();
}

void test_order_ids_fit_in_int32() {
    std::cout << "test_order_ids_fit_in_int32" << std::endl;
    Harness h;

    // OrderID.id is an int32 on the wire, and fill attribution compares it against
    // FillNotification's maker/taker ids.  An id above 2^31 would arrive negative.
    bool allFit = true;
    bool allNonZero = true;
    for (int i = 0; i < 5000; ++i) {
        unsigned int id = h.book.addLimitOrder(100.0 + (i % 50), 10, i % 2 == 0);
        if (id == 0) allNonZero = false;
        if (id > 2147483647u) allFit = false;
    }
    check(allFit, "5000 generated order ids all fit in a positive int32");
    check(allNonZero, "no generated order id is 0 (reserved for rejection)");
}

void test_bbo_size_matches_level2_top_level() {
    std::cout << "test_bbo_size_matches_level2_top_level" << std::endl;
    Harness h;

    h.book.addLimitOrder(100.0, 500, true);
    h.book.addLimitOrder(100.0, 250, true);   // two orders at the touch
    h.book.addLimitOrder(101.0, 400, false);

    auto sizes = h.book.getBidSizeAskSizeWithLock();
    const auto& askInfo = sizes.first;
    const auto& bidInfo = sizes.second;
    auto bids = h.book.getTopBidsWithLock(1);
    auto asks = h.book.getTopAsksWithLock(1);

    checkNear(bidInfo.first, 100.0, "BBO best bid price");
    checkEq(bidInfo.second, 750u, "BBO bid size aggregates the level, not just front-of-queue");
    checkEq(bidInfo.second, bids.at(0).second, "BBO bid size agrees with Level 2 rank 1");
    checkEq(askInfo.second, asks.at(0).second, "BBO ask size agrees with Level 2 rank 1");
}

}  // namespace

int main() {
    std::cout << "=== Order book invariant tests ===" << std::endl;
    test_depth_matches_resting_orders_on_add();
    test_depth_after_cancel();
    test_depth_after_partial_and_full_fill();
    test_cancel_after_fill_does_not_corrupt_depth();
    test_trade_prints_at_maker_price_buy_aggressor();
    test_trade_prints_at_maker_price_sell_aggressor();
    test_fill_stream_fans_out_to_every_subscriber();
    test_order_ids_fit_in_int32();
    test_bbo_size_matches_level2_top_level();

    if (failures == 0) {
        std::cout << "=== All order book invariant tests passed ===" << std::endl;
        return 0;
    }
    std::cout << "=== " << failures << " order book invariant assertion(s) FAILED ===" << std::endl;
    return 1;
}
