#pragma once

#include <cstdint>
#include <random>

// Deterministic per-instance random source.
//
// Each participant owns its own RandomSource seeded from config.randomSeed, so a
// fixed seed reproduces the same decision sequence.  This replaces the shared
// `static std::mt19937` in the legacy mean_reversion_client, which was a data
// race across the bid and ask threads.
//
// NOT thread-safe: give each thread its own RandomSource, or guard access.  The
// framework's design gives the single trading thread sole ownership.
class RandomSource {
public:
    explicit RandomSource(unsigned int seed) : gen_(seed) {}

    // Uniform integer in [lo, hi] inclusive.
    int intInRange(int lo, int hi) {
        std::uniform_int_distribution<int> dist(lo, hi);
        return dist(gen_);
    }

    // Uniform real in [lo, hi).
    double realInRange(double lo, double hi) {
        std::uniform_real_distribution<double> dist(lo, hi);
        return dist(gen_);
    }

    // Bernoulli trial: true with the given probability (clamped to [0,1]).
    bool chance(double probability) {
        if (probability <= 0.0) return false;
        if (probability >= 1.0) return true;
        std::bernoulli_distribution dist(probability);
        return dist(gen_);
    }

    // Size drawn around a mean with +/- variance (uniform), floored at 1.
    int sizeAround(int mean, int variance) {
        int lo = mean - variance;
        int hi = mean + variance;
        if (lo < 1) lo = 1;
        if (hi < lo) hi = lo;
        return intInRange(lo, hi);
    }

    std::mt19937& engine() { return gen_; }

private:
    std::mt19937 gen_;
};
