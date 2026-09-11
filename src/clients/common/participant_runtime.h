#pragma once

#include "engine_session.h"
#include "participant_config.h"
#include "strategy.h"

#include <memory>
#include <ostream>
#include <string>

// Owns everything mechanical about being a market participant, so a Strategy can
// contain nothing but behavior.
//
// Threads:
//   * fill stream   (EngineSession)  -> OrderTracker + PositionTracker + Strategy::onFill
//   * BBO stream    (EngineSession)  -> MarketState
//   * Level 2 poll  (here)           -> MarketState
//   * decision loop (calling thread) -> Strategy::onTick at decisionIntervalMs
//
// Shutdown is the part that matters for experiment hygiene: on SIGINT/SIGTERM, on
// maxRuntimeSeconds, or when the strategy reports completion, the runtime stops
// deciding, gives the strategy a final onStop, then cancels every order it still
// owns.  A participant that exits leaving live orders in the book silently poisons
// every subsequent measurement, so that outcome is a non-zero exit code.
class ParticipantRuntime {
public:
    ParticipantRuntime(ParticipantConfig config,
                       std::unique_ptr<Strategy> strategy,
                       std::ostream& eventOut);
    ~ParticipantRuntime();

    // Runs to completion. Returns a process exit code: 0 clean, non-zero otherwise.
    int run();

    // Async stop, safe to call from a signal handler context.
    static void requestShutdown();
    static bool shutdownRequested();

private:
    void pollLevel2Loop();
    MarketState snapshot() const;
    int finalize(EngineSession& engine, int exitCode);

    ParticipantConfig config_;
    std::unique_ptr<Strategy> strategy_;
    std::ostream& eventOut_;

    mutable std::mutex marketMutex_;
    MarketState market_;

    std::atomic<bool> pollRunning_{false};
    std::thread pollThread_;

    // Borrowed for the duration of run(); the session lives on run()'s stack.
    // Cleared before the session is destroyed so the poll thread cannot outlive it.
    std::atomic<EngineSession*> level2Engine_{nullptr};
};
