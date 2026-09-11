#include "tick_math.h"

#include <cmath>
#include <iostream>
#include <string>

namespace {

int failures = 0;

void check(bool cond, const std::string& what) {
    if (!cond) { std::cout << "  [FAIL] " << what << std::endl; ++failures; }
}

// These use EXACT double equality on purpose.
//
// Comparing with a tolerance is what hid a real bug: the ladder was submitting
// 500.34000000000003 where a config literal would say 500.34. Both "look" equal to
// any epsilon test, but the engine keys its order book on exact double equality, so
// they are two different price levels holding what should be one.
void test_from_index_matches_decimal_literals() {
    check(tickmath::fromIndex(50034, 0.01) == 500.34, "tick 50034 @0.01 == literal 500.34");
    check(tickmath::fromIndex(49999, 0.01) == 499.99, "tick 49999 @0.01 == literal 499.99");
    check(tickmath::fromIndex(50001, 0.01) == 500.01, "tick 50001 @0.01 == literal 500.01");
    check(tickmath::fromIndex(49954, 0.01) == 499.54, "tick 49954 @0.01 == literal 499.54");
    check(tickmath::fromIndex(0, 0.01) == 0.0, "tick 0 == 0.0");
    check(tickmath::fromIndex(-5, 0.01) == -0.05, "negative index works");
    std::cout << "  fromIndex matches decimal literals exactly" << std::endl;
}

void test_shift_matches_literals() {
    // Walking a ladder down from a reference must land on the same doubles a human
    // would write in a config file.
    check(tickmath::shift(500.00, -1, 0.01) == 499.99, "one tick below 500.00");
    check(tickmath::shift(500.00, 34, 0.01) == 500.34, "34 ticks above 500.00");
    check(tickmath::shift(500.00, -46, 0.01) == 499.54, "46 ticks below 500.00");
    check(tickmath::shift(499.99, 2, 0.01) == 500.01, "crossing a round number");
    std::cout << "  shift matches decimal literals exactly" << std::endl;
}

void test_round_trip_is_stable() {
    // Re-deriving a price from a price the engine handed back must be a fixed point,
    // or a ladder would drift a tick per rebuild.
    for (long long i = 49000; i <= 51000; ++i) {
        const double price = tickmath::fromIndex(i, 0.01);
        if (tickmath::toIndex(price, 0.01) != i) {
            check(false, "round trip failed at index " + std::to_string(i));
            return;
        }
        if (tickmath::round(price, 0.01) != price) {
            check(false, "round() is not idempotent at index " + std::to_string(i));
            return;
        }
    }
    std::cout << "  round trip is stable across 2000 ticks" << std::endl;
}

void test_other_tick_sizes() {
    check(tickmath::fromIndex(2005, 0.05) == 100.25, "0.05 tick size");
    check(tickmath::fromIndex(1234, 0.25) == 308.50, "0.25 tick size");
    check(tickmath::fromIndex(123456, 0.001) == 123.456, "0.001 tick size");
    check(tickmath::shift(100.00, 3, 0.05) == 100.15, "shift with a 0.05 tick");
    std::cout << "  non-penny tick sizes checked" << std::endl;
}

void test_rounding_snaps_to_grid() {
    check(tickmath::round(500.3400000001, 0.01) == 500.34, "snaps a near-miss up to grid");
    check(tickmath::round(499.98999999, 0.01) == 499.99, "snaps a near-miss down to grid");
    check(tickmath::toIndex(500.34000000000003, 0.01) == 50034, "contaminated price maps to right index");
    std::cout << "  grid snapping checked" << std::endl;
}

}  // namespace

void run_tick_math_tests() {
    std::cout << "--- TickMath Tests ---" << std::endl;
    test_from_index_matches_decimal_literals();
    test_shift_matches_literals();
    test_round_trip_is_stable();
    test_other_tick_sizes();
    test_rounding_snaps_to_grid();
    if (failures != 0) {
        std::cout << failures << " TickMath assertion(s) FAILED" << std::endl;
        std::exit(1);
    }
    std::cout << "All TickMath tests passed." << std::endl;
}
