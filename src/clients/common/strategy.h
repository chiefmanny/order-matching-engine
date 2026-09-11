#pragma once

#include "event_log.h"
#include "market_state.h"
#include "order_gateway.h"
#include "order_tracker.h"
#include "position_tracker.h"
#include "random_source.h"

#include <nlohmann/json.hpp>
#include <chrono>

// Everything a strategy is allowed to see and touch.
//
// Note what is absent: no gRPC, no threads, no sockets, no symbol registry.  A
// strategy answers behavioral questions only -- place or not, which side, what price,
// what size, passive or aggressive, cancel or leave -- and ParticipantRuntime owns
// every mechanical concern around it.
struct StrategyContext {
    const MarketState&     market;    // snapshot, stable for the duration of the call
    OrderTracker&          orders;
    PositionTracker&       position;
    RandomSource&          rng;       // seeded from config, owned by the decision thread
    OrderGateway&          engine;
    EventLog&              log;
    const nlohmann::json&  params;    // the `parameters` block of the participant config
    std::chrono::steady_clock::time_point now;
    double                 tickSize;
    double                 elapsedSeconds; // since this participant started
};

class Strategy {
public:
    virtual ~Strategy() = default;

    // Called once, after market data is flowing but before the first tick.
    virtual void onStart(StrategyContext&) {}

    // Called on a fixed cadence (config `decisionIntervalMs`).  The main hook.
    virtual void onTick(StrategyContext&) = 0;

    // Called for each execution against an order this participant owns.  Runs on the
    // fill-stream thread, not the decision thread -- keep it short and do not submit
    // orders from here; set state and let the next onTick act on it.
    virtual void onFill(StrategyContext&, const Fill&) {}

    // Last chance to act before shutdown.  The runtime cancels any remaining owned
    // orders afterwards regardless, so this is for reporting, not cleanup.
    virtual void onStop(StrategyContext&) {}

    // Strategy-specific numbers folded into the participant's final metrics.json.
    virtual nlohmann::json metrics() const { return nlohmann::json::object(); }

    // True once the strategy has finished its work and the runtime may shut down
    // early (a one-shot sweep, say).  Continuous strategies never return true.
    virtual bool isComplete() const { return false; }
};
