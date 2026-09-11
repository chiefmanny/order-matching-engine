#include "order_tracker.h"

#include <cassert>
#include <iostream>

static void test_accepted_order_becomes_active() {
    OrderTracker tracker;
    tracker.onAccepted(42, "AAPL", true, 499.5, 100);
    auto ids = tracker.activeOrderIds();
    assert(ids.size() == 1);
    assert(ids[0] == 42u);
    auto order = tracker.get(42);
    assert(order.has_value());
    assert(order->buy == true);
    assert(order->price == 499.5);
    assert(order->originalSize == 100);
    assert(order->remainingSize == 100);
    assert(order->state == OrderState::Active);
    std::cout << "PASS: " << __func__ << std::endl;
}

static void test_rejected_order_does_not_become_active() {
    OrderTracker tracker;
    tracker.onRejected();
    assert(tracker.activeCount() == 0);
    assert(tracker.activeOrderIds().empty());
    assert(tracker.rejectionCount() == 1);
    std::cout << "PASS: " << __func__ << std::endl;
}

static void test_full_fill_removes_active_order() {
    OrderTracker tracker;
    tracker.onAccepted(99, "AAPL", true, 500.0, 200);
    assert(tracker.onFill(99, 200) == true);
    assert(tracker.activeCount() == 0);
    auto order = tracker.get(99);
    assert(order.has_value());
    assert(order->state == OrderState::Filled);
    assert(order->remainingSize == 0);
    std::cout << "PASS: " << __func__ << std::endl;
}

static void test_partial_fill_reduces_remaining_size() {
    OrderTracker tracker;
    tracker.onAccepted(77, "AAPL", false, 501.0, 300);
    assert(tracker.onFill(77, 100) == true);
    auto order = tracker.get(77);
    assert(order.has_value());
    assert(order->remainingSize == 200);
    assert(order->state == OrderState::PartiallyFilled);
    assert(tracker.activeCount() == 1);
    std::cout << "PASS: " << __func__ << std::endl;
}

static void test_cancel_removes_active_order() {
    OrderTracker tracker;
    tracker.onAccepted(55, "AAPL", true, 498.0, 50);
    tracker.onCanceled(55);
    assert(tracker.activeCount() == 0);
    auto order = tracker.get(55);
    assert(order.has_value());
    assert(order->state == OrderState::Canceled);
    std::cout << "PASS: " << __func__ << std::endl;
}

static void test_unknown_cancel_is_safe() {
    OrderTracker tracker;
    tracker.onCanceled(99999); // no active order — must not throw or corrupt state
    assert(tracker.activeCount() == 0);
    std::cout << "PASS: " << __func__ << std::endl;
}

// The whole point of exact attribution: a fill for an id we do not own is ignored,
// no matter how plausible its price and size look.
static void test_fill_for_unowned_id_is_ignored() {
    OrderTracker tracker;
    tracker.onAccepted(11, "AAPL", true, 499.0, 100);
    assert(tracker.onFill(12345, 100) == false);
    assert(tracker.activeCount() == 1);
    auto order = tracker.get(11);
    assert(order->remainingSize == 100); // untouched
    std::cout << "PASS: " << __func__ << std::endl;
}

// Two participants resting at the SAME price and side used to defeat the old
// price-matching heuristic.  With ids, only the addressed order moves.
static void test_same_price_orders_attributed_individually() {
    OrderTracker tracker;
    tracker.onAccepted(1, "AAPL", true, 499.0, 100);
    tracker.onAccepted(2, "AAPL", true, 499.0, 100);

    assert(tracker.onFill(2, 60) == true);

    assert(tracker.get(1)->remainingSize == 100);
    assert(tracker.get(1)->state == OrderState::Active);
    assert(tracker.get(2)->remainingSize == 40);
    assert(tracker.get(2)->state == OrderState::PartiallyFilled);
    std::cout << "PASS: " << __func__ << std::endl;
}

static void test_fill_after_terminal_is_ignored() {
    OrderTracker tracker;
    tracker.onAccepted(7, "AAPL", false, 500.0, 100);
    assert(tracker.onFill(7, 100) == true);
    assert(tracker.onFill(7, 50) == false);  // stale duplicate notification
    assert(tracker.get(7)->remainingSize == 0);
    std::cout << "PASS: " << __func__ << std::endl;
}

static void test_overfill_is_clamped() {
    OrderTracker tracker;
    tracker.onAccepted(8, "AAPL", true, 500.0, 100);
    assert(tracker.onFill(8, 250) == true);   // engine should never do this; be safe
    assert(tracker.get(8)->remainingSize == 0);
    assert(tracker.get(8)->state == OrderState::Filled);
    assert(tracker.stats().sharesFilled == 100); // clamped, not 250
    std::cout << "PASS: " << __func__ << std::endl;
}

static void test_ownership_checks() {
    OrderTracker tracker;
    tracker.onAccepted(21, "AAPL", true, 499.0, 100);

    assert(tracker.owns(21));
    assert(tracker.isLiveOrder(21));
    assert(!tracker.owns(22));          // never submitted by us
    assert(!tracker.isLiveOrder(22));

    tracker.onCanceled(21);
    assert(tracker.owns(21));           // still ours, historically
    assert(!tracker.isLiveOrder(21));   // but no longer actionable
    std::cout << "PASS: " << __func__ << std::endl;
}

static void test_pending_cancel_still_accepts_fills() {
    OrderTracker tracker;
    tracker.onAccepted(31, "AAPL", true, 499.0, 100);
    tracker.markPendingCancel(31);
    // A cancel racing an execution is normal; the fill must still be counted.
    assert(tracker.onFill(31, 100) == true);
    assert(tracker.get(31)->state == OrderState::Filled);
    std::cout << "PASS: " << __func__ << std::endl;
}

static void test_multiple_active_orders() {
    OrderTracker tracker;
    tracker.onAccepted(1, "AAPL", true, 499.0, 100);
    tracker.onAccepted(2, "AAPL", true, 498.0, 200);
    tracker.onAccepted(3, "AAPL", false, 501.0, 150);
    assert(tracker.activeCount() == 3);
    tracker.onCanceled(2);
    assert(tracker.activeCount() == 2);
    auto ids = tracker.activeOrderIds();
    assert(ids.size() == 2u);
    std::cout << "PASS: " << __func__ << std::endl;
}

static void test_multiple_rejections_counted() {
    OrderTracker tracker;
    tracker.onRejected();
    tracker.onRejected();
    tracker.onRejected();
    assert(tracker.rejectionCount() == 3);
    assert(tracker.activeCount() == 0);
    std::cout << "PASS: " << __func__ << std::endl;
}

static void test_stats_summarize_order_flow() {
    OrderTracker tracker;
    tracker.onAccepted(1, "AAPL", true, 499.0, 100);
    tracker.onAccepted(2, "AAPL", true, 498.0, 200);
    tracker.onAccepted(3, "AAPL", false, 501.0, 150);
    tracker.onRejected();

    tracker.onFill(1, 40);    // partial
    tracker.onFill(1, 60);    // completes order 1
    tracker.onCanceled(2);

    auto s = tracker.stats();
    assert(s.submitted == 3);
    assert(s.rejected == 1);
    assert(s.canceled == 1);
    assert(s.filled == 1);
    assert(s.fillEvents == 2);
    assert(s.sharesFilled == 100);
    assert(s.meanOrderLifetimeSeconds >= 0.0);
    std::cout << "PASS: " << __func__ << std::endl;
}

void run_order_tracker_tests() {
    std::cout << "--- OrderTracker Tests ---" << std::endl;
    test_accepted_order_becomes_active();
    test_rejected_order_does_not_become_active();
    test_full_fill_removes_active_order();
    test_partial_fill_reduces_remaining_size();
    test_cancel_removes_active_order();
    test_unknown_cancel_is_safe();
    test_fill_for_unowned_id_is_ignored();
    test_same_price_orders_attributed_individually();
    test_fill_after_terminal_is_ignored();
    test_overfill_is_clamped();
    test_ownership_checks();
    test_pending_cancel_still_accepts_fills();
    test_multiple_active_orders();
    test_multiple_rejections_counted();
    test_stats_summarize_order_flow();
    std::cout << "All OrderTracker tests passed." << std::endl;
}
