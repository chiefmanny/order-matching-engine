#pragma once

#include <nlohmann/json.hpp>
#include <string>

struct ParticipantConfig {
    std::string participantId;
    std::string runId;
    std::string symbol;
    std::string strategy;
    std::string serverUri        = "localhost:50051";
    unsigned int randomSeed      = 0;
    int maxRuntimeSeconds        = 300;
    int shutdownTimeoutMs        = 5000;

    // Runtime cadence and market conventions.  These belong to the participant
    // runtime rather than to any one strategy, so they sit alongside the identity
    // fields rather than inside `parameters`.
    double tickSize              = 0.01;  // price grid; strategies must round to it
    int decisionIntervalMs       = 100;   // how often Strategy::onTick fires
    int level2Depth              = 15;    // levels pulled per GetLevel2 poll
    int level2IntervalMs         = 250;   // how often the local book snapshot refreshes

    // Where structured JSONL events go.  Empty means stdout, which is what the
    // experiment runner captures.
    std::string eventLogPath;

    nlohmann::json parameters    = nlohmann::json::object();

    // Load from a JSON file.  Throws std::runtime_error on missing required
    // fields or an unreadable file.
    static ParticipantConfig fromFile(const std::string& path);

    // Parse CLI arguments.  Requires --config <path>.  Optional overrides:
    //   --strategy, --server, --participant-id, --run-id, --seed
    static ParticipantConfig fromArgs(int argc, char** argv);
};
