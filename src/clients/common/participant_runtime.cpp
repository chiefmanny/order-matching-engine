#include "participant_runtime.h"

#include <csignal>
#include <fstream>
#include <iostream>
#include <utility>

namespace {

// Set from a signal handler, so it must be a lock-free integral flag.
std::atomic<bool> g_shutdown{false};

void handleSignal(int) { g_shutdown = true; }

// Exit codes the experiment runner distinguishes.
constexpr int kOk                  = 0;
constexpr int kEngineUnreachable   = 2;
constexpr int kSymbolNotFound      = 3;
constexpr int kOrdersLeftBehind    = 4;

}  // namespace

void ParticipantRuntime::requestShutdown() { g_shutdown = true; }
bool ParticipantRuntime::shutdownRequested() { return g_shutdown.load(); }

ParticipantRuntime::ParticipantRuntime(ParticipantConfig config,
                                       std::unique_ptr<Strategy> strategy,
                                       std::ostream& eventOut)
    : config_(std::move(config)),
      strategy_(std::move(strategy)),
      eventOut_(eventOut) {}

ParticipantRuntime::~ParticipantRuntime() {
    pollRunning_ = false;
    if (pollThread_.joinable()) pollThread_.join();
}

MarketState ParticipantRuntime::snapshot() const {
    std::lock_guard<std::mutex> lock(marketMutex_);
    return market_;
}

int ParticipantRuntime::run() {
    std::signal(SIGINT, handleSignal);
    std::signal(SIGTERM, handleSignal);

    EventLog log(config_.runId, config_.participantId, eventOut_);
    OrderTracker tracker;
    PositionTracker position;
    position.symbol = config_.symbol;
    RandomSource rng(config_.randomSeed);

    log.emit("start", {{"strategy", config_.strategy},
                       {"symbol", config_.symbol},
                       {"seed", config_.randomSeed},
                       {"server", config_.serverUri},
                       {"tick_size", config_.tickSize},
                       {"decision_interval_ms", config_.decisionIntervalMs},
                       {"max_runtime_seconds", config_.maxRuntimeSeconds}});

    EngineSession engine(config_.serverUri, config_.symbol, tracker, log);

    switch (engine.waitUntilReady(std::chrono::seconds(30))) {
        case EngineSession::Readiness::Ready:
            break;
        case EngineSession::Readiness::SymbolNotFound:
            log.emit("fatal", {{"reason", "symbol_not_registered"},
                               {"symbol", config_.symbol}});
            std::cerr << "Symbol " << config_.symbol << " is not registered on the engine.\n";
            return kSymbolNotFound;
        case EngineSession::Readiness::Timeout:
            log.emit("fatal", {{"reason", "engine_unreachable"},
                               {"server", config_.serverUri}});
            std::cerr << "Could not reach the engine at " << config_.serverUri << ".\n";
            return kEngineUnreachable;
    }
    log.emit("connected", {{"server", config_.serverUri}});

    // --- market data -------------------------------------------------------
    engine.startBboStream([this](double bid, int bidSize, double ask, int askSize) {
        std::lock_guard<std::mutex> lock(marketMutex_);
        market_.bestBid     = bid;
        market_.bestBidSize = bidSize;
        market_.bestAsk     = ask;
        market_.bestAskSize = askSize;
        market_.updatedAt   = std::chrono::steady_clock::now();
    });

    pollRunning_ = true;
    pollThread_ = std::thread(&ParticipantRuntime::pollLevel2Loop, this);
    // pollLevel2Loop needs the session; hand it over via a member-free capture by
    // storing a pointer for the duration of the run.
    level2Engine_ = &engine;

    // --- fills -------------------------------------------------------------
    // Runs on the fill-stream thread. Position and last price update here so the
    // next onTick sees them; the strategy hook is expected to be short.
    engine.startFillStream([&](const Fill& fill) {
        position.onFill(fill.buy, fill.size, fill.price);
        {
            std::lock_guard<std::mutex> lock(marketMutex_);
            market_.lastPrice = fill.price;
        }
        MarketState snap = snapshot();
        StrategyContext ctx{snap, tracker, position, rng, engine, log,
                            config_.parameters, std::chrono::steady_clock::now(),
                            config_.tickSize, 0.0};
        strategy_->onFill(ctx, fill);
    });

    // --- decision loop -----------------------------------------------------
    const auto startedAt = std::chrono::steady_clock::now();
    const auto deadline  = startedAt + std::chrono::seconds(config_.maxRuntimeSeconds);
    const auto interval  = std::chrono::milliseconds(config_.decisionIntervalMs);

    {
        MarketState snap = snapshot();
        StrategyContext ctx{snap, tracker, position, rng, engine, log,
                            config_.parameters, std::chrono::steady_clock::now(),
                            config_.tickSize, 0.0};
        strategy_->onStart(ctx);
    }

    std::string stopReason = "max_runtime";
    while (!g_shutdown) {
        if (std::chrono::steady_clock::now() >= deadline) break;
        if (strategy_->isComplete()) { stopReason = "strategy_complete"; break; }

        const auto tickStart = std::chrono::steady_clock::now();
        MarketState snap = snapshot();
        StrategyContext ctx{snap, tracker, position, rng, engine, log,
                            config_.parameters, tickStart, config_.tickSize,
                            std::chrono::duration<double>(tickStart - startedAt).count()};
        strategy_->onTick(ctx);

        // Fixed cadence: sleep the remainder of the interval, not the whole interval,
        // so a slow tick does not compound into drift.
        const auto elapsed = std::chrono::steady_clock::now() - tickStart;
        if (elapsed < interval) std::this_thread::sleep_for(interval - elapsed);
    }
    if (g_shutdown) stopReason = "signal";

    log.emit("stopping", {{"reason", stopReason}});

    {
        MarketState snap = snapshot();
        StrategyContext ctx{snap, tracker, position, rng, engine, log,
                            config_.parameters, std::chrono::steady_clock::now(),
                            config_.tickSize,
                            std::chrono::duration<double>(
                                std::chrono::steady_clock::now() - startedAt).count()};
        strategy_->onStop(ctx);
    }

    // --- shutdown ----------------------------------------------------------
    pollRunning_ = false;
    if (pollThread_.joinable()) pollThread_.join();
    level2Engine_ = nullptr;

    // Cancel owned orders, retrying until the shutdown budget is spent. Retries
    // matter because a cancel can race an in-flight match.
    const auto cancelDeadline =
        std::chrono::steady_clock::now() + std::chrono::milliseconds(config_.shutdownTimeoutMs);
    int remaining = engine.cancelAllOwned();
    while (remaining > 0 && std::chrono::steady_clock::now() < cancelDeadline) {
        std::this_thread::sleep_for(std::chrono::milliseconds(100));
        remaining = engine.cancelAllOwned();
    }

    int exitCode = kOk;
    if (remaining > 0) {
        log.emit("fatal", {{"reason", "orders_left_behind"}, {"count", remaining}});
        std::cerr << "Shutdown left " << remaining << " live order(s) in the book.\n";
        exitCode = kOrdersLeftBehind;
    }

    // Final metrics before the streams are torn down, so counters are stable.
    const auto stats = tracker.stats();
    const auto& c = engine.counters();
    nlohmann::json metrics = {
        {"stop_reason", stopReason},
        {"runtime_seconds", std::chrono::duration<double>(
                                std::chrono::steady_clock::now() - startedAt).count()},
        {"orders_submitted", stats.submitted},
        {"orders_rejected", stats.rejected},
        {"orders_canceled", stats.canceled},
        {"orders_filled", stats.filled},
        {"fill_events", stats.fillEvents},
        {"shares_filled", stats.sharesFilled},
        {"mean_order_lifetime_seconds", stats.meanOrderLifetimeSeconds},
        {"orders_left_live", remaining},
        {"inventory", position.inventory},
        {"realized_pnl", position.realizedPnl},
        {"average_entry_price", position.averageEntryPrice},
        {"buy_shares_filled", position.buySharesFilled},
        {"sell_shares_filled", position.sellSharesFilled},
        {"fills_seen_on_symbol", c.fillsSeen.load()},
        {"fills_attributed_to_us", c.fillsOurs.load()},
        {"bbo_updates", c.bboUpdates.load()},
        {"level2_polls", c.level2Polls.load()},
        {"rpc_errors", c.rpcErrors.load()},
        {"ownership_violations", c.ownershipViolations.load()},
        {"strategy", strategy_->metrics()},
    };
    const MarketState finalMarket = snapshot();
    metrics["unrealized_pnl"] =
        finalMarket.lastPrice > 0.0 ? position.unrealizedPnl(finalMarket.lastPrice) : 0.0;

    log.emit("metrics", metrics);

    engine.stop();
    return exitCode;
}

void ParticipantRuntime::pollLevel2Loop() {
    std::vector<PriceLevel> bids, asks;
    const auto interval = std::chrono::milliseconds(config_.level2IntervalMs);

    while (pollRunning_) {
        EngineSession* engine = level2Engine_;
        if (engine && engine->pollLevel2(config_.level2Depth, bids, asks)) {
            std::lock_guard<std::mutex> lock(marketMutex_);
            market_.bidLevels = bids;
            market_.askLevels = asks;
            market_.updatedAt = std::chrono::steady_clock::now();
        }
        // Poll in small slices so shutdown is prompt even with a long interval.
        for (auto waited = std::chrono::milliseconds(0);
             waited < interval && pollRunning_;
             waited += std::chrono::milliseconds(20)) {
            std::this_thread::sleep_for(std::chrono::milliseconds(20));
        }
    }
}
