//
//  participant_runner.cpp
//
//  The single participant executable.  Which market participant this process becomes
//  is decided entirely by config: `strategy` selects an entry from the registry
//  below, and `parameters` configures it.
//
//  Adding a participant is one new strategy .cpp plus one line in kRegistry -- never
//  a new binary, a new CMake target, or a new Dockerfile COPY.
//
//  Usage:
//    participant_runner --config path/to/participant.json
//                       [--strategy X] [--server host:port]
//                       [--participant-id ID] [--run-id ID] [--seed N]
//                       [--list-strategies]
//

#include "participant_config.h"
#include "participant_runtime.h"
#include "strategy.h"
#include "strategies/echo_strategy.h"
#include "strategies/layering_demo.h"
#include "strategies/liquidity_provider.h"
#include "strategies/directional_trader.h"
#include "strategies/market_maker.h"
#include "strategies/noise_trader.h"
#include "strategies/parent_executor.h"

#include <fstream>
#include <functional>
#include <iostream>
#include <map>
#include <memory>
#include <string>

namespace {

using StrategyFactory = std::function<std::unique_ptr<Strategy>()>;

const std::map<std::string, StrategyFactory>& registry() {
    static const std::map<std::string, StrategyFactory> kRegistry = {
        {"echo", [] { return std::make_unique<EchoStrategy>(); }},
        {"layering_demo", [] { return std::make_unique<LayeringDemo>(); }},
        {"liquidity_provider", [] { return std::make_unique<LiquidityProvider>(); }},
        {"directional_trader", [] { return std::make_unique<DirectionalTrader>(); }},
        {"market_maker", [] { return std::make_unique<MarketMaker>(); }},
        {"noise_trader", [] { return std::make_unique<NoiseTrader>(); }},
        {"parent_executor", [] { return std::make_unique<ParentExecutor>(); }},
        // `sweep` is the immediate special case of a parent order, kept as an alias
        // so Phase 6 scenarios keep working rather than being rewritten.
        {"sweep", [] { return std::make_unique<ParentExecutor>(); }},
        // Phase 11+: parent_executor and later actors each add one line here.
    };
    return kRegistry;
}

void printStrategies(std::ostream& out) {
    out << "Available strategies:\n";
    for (const auto& entry : registry()) {
        out << "  " << entry.first << "\n";
    }
}

}  // namespace

int main(int argc, char** argv) {
    for (int i = 1; i < argc; ++i) {
        const std::string arg = argv[i];
        if (arg == "--list-strategies") {
            printStrategies(std::cout);
            return 0;
        }
        if (arg == "--help" || arg == "-h") {
            std::cout << "Usage: " << argv[0]
                      << " --config <path> [--strategy X] [--server host:port]"
                         " [--participant-id ID] [--run-id ID] [--seed N]\n";
            printStrategies(std::cout);
            return 0;
        }
    }

    ParticipantConfig config;
    try {
        config = ParticipantConfig::fromArgs(argc, argv);
    } catch (const std::exception& e) {
        std::cerr << "Config error: " << e.what() << "\n";
        return 1;
    }

    auto it = registry().find(config.strategy);
    if (it == registry().end()) {
        std::cerr << "Unknown strategy: " << config.strategy << "\n";
        printStrategies(std::cerr);
        return 1;
    }

    // Structured events go to the configured sink; human-readable messages go to
    // stderr, so the harness can consume stdout as pure JSONL.
    std::ofstream fileSink;
    if (!config.eventLogPath.empty()) {
        fileSink.open(config.eventLogPath, std::ios::out | std::ios::trunc);
        if (!fileSink) {
            std::cerr << "Cannot open event log: " << config.eventLogPath << "\n";
            return 1;
        }
    }
    std::ostream& eventOut = config.eventLogPath.empty()
                                 ? static_cast<std::ostream&>(std::cout)
                                 : static_cast<std::ostream&>(fileSink);

    ParticipantRuntime runtime(config, it->second(), eventOut);
    return runtime.run();
}
