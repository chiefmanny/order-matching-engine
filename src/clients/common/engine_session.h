#pragma once

#include "event_log.h"
#include "market_state.h"
#include "order_gateway.h"
#include "order_tracker.h"

#include "matching_engine.grpc.pb.h"
#include <grpcpp/channel.h>
#include <grpcpp/client_context.h>
#include <grpcpp/create_channel.h>

#include <atomic>
#include <chrono>
#include <functional>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

// One participant's connection to the matching engine.
//
// This is the ONLY place in a participant that knows gRPC exists.  It owns its own
// stub rather than reusing MatchingEngineClient, which is blocking, prints to stdout,
// bounds its streams by a wall-clock duration, and writes its own CSV files -- none of
// which suits a long-lived participant under harness control.  MatchingEngineClient
// is left untouched for the legacy demo client.
//
// Ownership enforcement lives here: the engine performs no ownership check at all, so
// cancel/replace consult the OrderTracker and refuse ids this participant did not
// submit.  Attempts are counted and logged rather than silently dropped.
class EngineSession : public OrderGateway {
public:
    EngineSession(const std::string& uri,
                  std::string symbolIn,
                  OrderTracker& tracker,
                  EventLog& log);
    ~EngineSession() override;

    EngineSession(const EngineSession&) = delete;
    EngineSession& operator=(const EngineSession&) = delete;

    // Blocks until the engine answers CheckSymbol for our symbol, or the timeout
    // expires.  Distinguishes "engine not up yet" from "symbol not registered":
    // the latter is fatal and returns immediately.
    enum class Readiness { Ready, Timeout, SymbolNotFound };
    Readiness waitUntilReady(std::chrono::milliseconds timeout);

    // --- OrderGateway ------------------------------------------------------
    unsigned int submit(bool buy, double price, int size) override;
    bool cancel(unsigned int orderId) override;
    unsigned int replace(unsigned int orderId, double price, int size) override;
    const std::string& symbol() const override { return symbol_; }

    // --- Market data -------------------------------------------------------
    using FillCallback = std::function<void(const Fill&)>;
    using BboCallback = std::function<void(double bid, int bidSize, double ask, int askSize)>;

    // Each starts a background thread. Safe to call once each, before run.
    void startFillStream(FillCallback cb);
    void startBboStream(BboCallback cb);

    // Synchronous snapshot. GetLevel2 is declared as a stream but the server writes
    // one response and returns, so this is a poll, not a subscription.
    bool pollLevel2(int depth, std::vector<PriceLevel>& bids, std::vector<PriceLevel>& asks);

    // Cancels every order this participant still has live.  Returns the number that
    // remain live afterwards -- a non-zero result means the participant is leaking
    // orders into the book and the run should be marked failed.
    int cancelAllOwned();

    // Cancels in-flight streams and joins their threads.  Idempotent.
    void stop();

    struct Counters {
        std::atomic<uint64_t> fillsSeen{0};      // every fill on the symbol
        std::atomic<uint64_t> fillsOurs{0};      // fills attributed to our orders
        std::atomic<uint64_t> bboUpdates{0};
        std::atomic<uint64_t> level2Polls{0};
        std::atomic<uint64_t> rpcErrors{0};
        std::atomic<uint64_t> ownershipViolations{0};
        std::atomic<bool>     fillStreamEnded{false};
        std::atomic<bool>     bboStreamEnded{false};
    };
    const Counters& counters() const { return counters_; }

private:
    void runFillStream(FillCallback cb);
    void runBboStream(BboCallback cb);

    std::string symbol_;
    OrderTracker& tracker_;
    EventLog& log_;
    std::shared_ptr<grpc::Channel> channel_;
    std::unique_ptr<matchingengine::MatchingEngine::Stub> stub_;

    std::atomic<bool> stopping_{false};
    std::thread fillThread_;
    std::thread bboThread_;

    // Held so stop() can TryCancel a blocked stream from another thread.
    std::mutex contextMutex_;
    grpc::ClientContext* fillContext_ = nullptr;
    grpc::ClientContext* bboContext_ = nullptr;

    Counters counters_;
};
