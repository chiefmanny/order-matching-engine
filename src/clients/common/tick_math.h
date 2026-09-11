#pragma once

#include <cmath>

// Price-grid helpers.
//
// Every strategy must round prices to the tick grid before submitting. The engine
// keys its book on exact double equality, so an unrounded price does not "snap" to a
// nearby level -- it silently creates a brand new price level per floating-point
// wobble, fragmenting displayed depth in a way that is invisible until Level 2 looks
// subtly wrong.
namespace tickmath {

// Integer index of a price on the tick grid. Safe as a map key, unlike a double.
inline long long toIndex(double price, double tickSize) {
    return std::llround(price / tickSize);
}

// Index -> price.
//
// The rounding step is load-bearing, not cosmetic. `50034 * 0.01` evaluates to
// 500.34000000000003, which is a DIFFERENT double from the literal 500.34 that a
// config file or another code path would produce. The engine keys its book on exact
// double equality, so those two would become two separate price levels holding what
// is meant to be one -- fragmenting displayed depth and corrupting Level 2.
//
// Snapping to 1e-9 lands on the same double a literal decimal produces, so every
// producer of "the price at tick N" agrees exactly.
inline double fromIndex(long long index, double tickSize) {
    return std::round(static_cast<double>(index) * tickSize * 1e9) / 1e9;
}

inline double round(double price, double tickSize) {
    return fromIndex(toIndex(price, tickSize), tickSize);
}

// Shifts a price by a whole number of ticks, staying on the grid.
inline double shift(double price, long long ticks, double tickSize) {
    return fromIndex(toIndex(price, tickSize) + ticks, tickSize);
}

}  // namespace tickmath
