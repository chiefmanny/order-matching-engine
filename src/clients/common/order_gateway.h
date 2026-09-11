#pragma once

#include <cstdint>
#include <string>

// The only way a strategy is allowed to touch the market.
//
// This is deliberately a narrow abstract interface rather than the concrete
// EngineSession, for one concrete reason: it keeps strategies free of gRPC, so
// liquidity_provider / market_maker / sweep logic can be unit-tested against a fake
// gateway in the fast `participant_tests` target, with no engine and no network.
// EngineSession is the single real implementation.
//
// Ownership rule: an implementation MUST refuse to cancel or replace an order this
// participant did not submit.  The matching engine performs no ownership check of its
// own, so this interface is the only thing standing between a buggy strategy and
// another participant's resting orders.
class OrderGateway {
public:
    virtual ~OrderGateway() = default;

    // Submits a limit order.  Returns the engine's order id, or 0 if rejected.
    virtual unsigned int submit(bool buy, double price, int size) = 0;

    // Cancels an order this participant owns.  False if unknown, already
    // terminal, or not ours.
    virtual bool cancel(unsigned int orderId) = 0;

    // Cancel-and-resubmit.  The engine exposes no modify RPC, and cancel/replace is
    // the realistic behavior anyway (it surrenders queue position).  Returns the new
    // order id, or 0 if the cancel failed or the new order was rejected.
    virtual unsigned int replace(unsigned int orderId, double price, int size) = 0;

    virtual const std::string& symbol() const = 0;
};

// One execution against an order this participant owns.
struct Fill {
    unsigned int orderId = 0;
    double       price   = 0.0;
    int          size    = 0;
    bool         buy     = false;  // side of OUR order, not the aggressor
    bool         isMaker = false;  // our order was resting when it traded
    int64_t      tsNs    = 0;      // engine-side match time
};
