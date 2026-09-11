#pragma once

#include "order_gateway.h"
#include "order_tracker.h"

#include <string>
#include <vector>

// In-memory stand-in for EngineSession, so strategy behavior can be tested with no
// engine, no gRPC and no threads.  It keeps an OrderTracker in step exactly the way
// EngineSession does, which is what lets a strategy's isLiveOrder() checks behave
// realistically, and it records every call so tests can assert on order flow.
class FakeGateway : public OrderGateway {
public:
    struct Submission {
        unsigned int id;
        bool         buy;
        double       price;
        int          size;
    };

    explicit FakeGateway(OrderTracker& tracker, std::string sym = "TEST")
        : tracker_(tracker), symbol_(std::move(sym)) {}

    unsigned int submit(bool buy, double price, int size) override {
        if (rejectNext_ > 0) {
            --rejectNext_;
            tracker_.onRejected();
            rejected.push_back(Submission{0, buy, price, size});
            return 0;
        }
        const unsigned int id = nextId_++;
        tracker_.onAccepted(id, symbol_, buy, price, size);
        submissions.push_back(Submission{id, buy, price, size});
        return id;
    }

    bool cancel(unsigned int orderId) override {
        // Mirrors EngineSession's ownership gate.
        if (!tracker_.owns(orderId)) {
            ownershipViolations.push_back(orderId);
            return false;
        }
        if (!tracker_.isLiveOrder(orderId)) return false;
        tracker_.onCanceled(orderId);
        canceled.push_back(orderId);
        return true;
    }

    unsigned int replace(unsigned int orderId, double price, int size) override {
        auto prior = tracker_.get(orderId);
        if (!cancel(orderId)) return 0;
        return submit(prior.has_value() ? prior->buy : false, price, size);
    }

    const std::string& symbol() const override { return symbol_; }

    // --- test controls -----------------------------------------------------
    void rejectNextSubmissions(int n) { rejectNext_ = n; }

    // Simulates the engine executing against a resting order.
    void fill(unsigned int orderId, int size) { tracker_.onFill(orderId, size); }

    std::vector<Submission>   submissions;
    std::vector<Submission>   rejected;
    std::vector<unsigned int> canceled;
    std::vector<unsigned int> ownershipViolations;

private:
    OrderTracker& tracker_;
    std::string   symbol_;
    unsigned int  nextId_    = 1000;
    int           rejectNext_ = 0;
};
