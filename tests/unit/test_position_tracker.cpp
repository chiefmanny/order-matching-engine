#include "position_tracker.h"

#include <cassert>
#include <cmath>
#include <iostream>

static bool approx(double a, double b, double eps = 1e-9) {
    return std::fabs(a - b) < eps;
}

static void test_buy_increases_inventory() {
    PositionTracker p;
    p.onFill(true, 100, 500.0);
    assert(p.inventory == 100);
    assert(approx(p.averageEntryPrice, 500.0));
    assert(p.buySharesFilled == 100);
    assert(p.sellSharesFilled == 0);
    assert(approx(p.realizedPnl, 0.0));
    std::cout << "PASS: " << __func__ << std::endl;
}

static void test_sell_decreases_inventory() {
    PositionTracker p;
    p.onFill(false, 50, 500.0);
    assert(p.inventory == -50);
    assert(approx(p.averageEntryPrice, 500.0));
    assert(p.sellSharesFilled == 50);
    std::cout << "PASS: " << __func__ << std::endl;
}

static void test_average_entry_price_on_multiple_buys() {
    PositionTracker p;
    p.onFill(true, 100, 500.0);
    p.onFill(true, 100, 502.0);
    assert(p.inventory == 200);
    assert(approx(p.averageEntryPrice, 501.0)); // (500*100 + 502*100)/200
    std::cout << "PASS: " << __func__ << std::endl;
}

static void test_realized_pnl_on_long_close() {
    PositionTracker p;
    p.onFill(true, 100, 500.0);  // long 100 @ 500
    p.onFill(false, 100, 510.0); // sell 100 @ 510 -> +10*100 = 1000
    assert(p.inventory == 0);
    assert(approx(p.realizedPnl, 1000.0));
    assert(approx(p.averageEntryPrice, 0.0));
    std::cout << "PASS: " << __func__ << std::endl;
}

static void test_realized_pnl_on_short_close() {
    PositionTracker p;
    p.onFill(false, 100, 500.0); // short 100 @ 500
    p.onFill(true, 100, 490.0);  // buy 100 @ 490 -> +10*100 = 1000
    assert(p.inventory == 0);
    assert(approx(p.realizedPnl, 1000.0));
    std::cout << "PASS: " << __func__ << std::endl;
}

static void test_partial_close_keeps_average() {
    PositionTracker p;
    p.onFill(true, 200, 500.0); // long 200 @ 500
    p.onFill(false, 50, 510.0); // sell 50 @ 510 -> +10*50 = 500
    assert(p.inventory == 150);
    assert(approx(p.averageEntryPrice, 500.0)); // unchanged on reduce
    assert(approx(p.realizedPnl, 500.0));
    std::cout << "PASS: " << __func__ << std::endl;
}

static void test_position_flip_realizes_and_rebases() {
    PositionTracker p;
    p.onFill(true, 100, 500.0);  // long 100 @ 500
    p.onFill(false, 150, 510.0); // sell 150 @ 510: close 100 (+10*100=1000), open short 50 @ 510
    assert(p.inventory == -50);
    assert(approx(p.realizedPnl, 1000.0));
    assert(approx(p.averageEntryPrice, 510.0)); // rebased to fill price
    std::cout << "PASS: " << __func__ << std::endl;
}

static void test_unrealized_pnl() {
    PositionTracker p;
    p.onFill(true, 100, 500.0);           // long 100 @ 500
    assert(approx(p.unrealizedPnl(505.0), 500.0)); // (505-500)*100
    assert(approx(p.unrealizedPnl(495.0), -500.0));
    std::cout << "PASS: " << __func__ << std::endl;
}

static void test_unrealized_pnl_flat_is_zero() {
    PositionTracker p;
    assert(approx(p.unrealizedPnl(999.0), 0.0));
    std::cout << "PASS: " << __func__ << std::endl;
}

void run_position_tracker_tests() {
    std::cout << "--- PositionTracker Tests ---" << std::endl;
    test_buy_increases_inventory();
    test_sell_decreases_inventory();
    test_average_entry_price_on_multiple_buys();
    test_realized_pnl_on_long_close();
    test_realized_pnl_on_short_close();
    test_partial_close_keeps_average();
    test_position_flip_realizes_and_rebases();
    test_unrealized_pnl();
    test_unrealized_pnl_flat_is_zero();
    std::cout << "All PositionTracker tests passed." << std::endl;
}
