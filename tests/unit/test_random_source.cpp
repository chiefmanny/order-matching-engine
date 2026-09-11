#include "random_source.h"

#include <cassert>
#include <iostream>
#include <vector>

static void test_same_seed_same_sequence() {
    RandomSource a(42);
    RandomSource b(42);
    for (int i = 0; i < 100; ++i) {
        assert(a.intInRange(0, 1000000) == b.intInRange(0, 1000000));
    }
    std::cout << "PASS: " << __func__ << std::endl;
}

static void test_different_seed_different_sequence() {
    RandomSource a(1);
    RandomSource b(2);
    bool anyDifferent = false;
    for (int i = 0; i < 100; ++i) {
        if (a.intInRange(0, 1000000) != b.intInRange(0, 1000000)) {
            anyDifferent = true;
            break;
        }
    }
    assert(anyDifferent);
    std::cout << "PASS: " << __func__ << std::endl;
}

static void test_int_in_range_bounds() {
    RandomSource r(7);
    for (int i = 0; i < 1000; ++i) {
        int v = r.intInRange(10, 20);
        assert(v >= 10 && v <= 20);
    }
    std::cout << "PASS: " << __func__ << std::endl;
}

static void test_real_in_range_bounds() {
    RandomSource r(7);
    for (int i = 0; i < 1000; ++i) {
        double v = r.realInRange(1.0, 2.0);
        assert(v >= 1.0 && v < 2.0);
    }
    std::cout << "PASS: " << __func__ << std::endl;
}

static void test_chance_extremes() {
    RandomSource r(7);
    for (int i = 0; i < 100; ++i) {
        assert(r.chance(0.0) == false);
        assert(r.chance(1.0) == true);
    }
    std::cout << "PASS: " << __func__ << std::endl;
}

static void test_size_around_floored_at_one() {
    RandomSource r(7);
    for (int i = 0; i < 1000; ++i) {
        int v = r.sizeAround(3, 10); // could go negative before flooring
        assert(v >= 1);
    }
    std::cout << "PASS: " << __func__ << std::endl;
}

static void test_size_around_within_range() {
    RandomSource r(7);
    for (int i = 0; i < 1000; ++i) {
        int v = r.sizeAround(100, 20);
        assert(v >= 80 && v <= 120);
    }
    std::cout << "PASS: " << __func__ << std::endl;
}

void run_random_source_tests() {
    std::cout << "--- RandomSource Tests ---" << std::endl;
    test_same_seed_same_sequence();
    test_different_seed_different_sequence();
    test_int_in_range_bounds();
    test_real_in_range_bounds();
    test_chance_extremes();
    test_size_around_floored_at_one();
    test_size_around_within_range();
    std::cout << "All RandomSource tests passed." << std::endl;
}
