#include "participant_config.h"

#include <fstream>
#include <stdexcept>
#include <string>

ParticipantConfig ParticipantConfig::fromFile(const std::string& path) {
    std::ifstream f(path);
    if (!f.is_open()) {
        throw std::runtime_error("Cannot open config file: " + path);
    }

    nlohmann::json j;
    try {
        j = nlohmann::json::parse(f);
    } catch (const nlohmann::json::parse_error& e) {
        throw std::runtime_error(std::string("JSON parse error in ") + path + ": " + e.what());
    }

    ParticipantConfig cfg;
    // Required fields — j.at() throws nlohmann::json::out_of_range if missing.
    try {
        cfg.participantId = j.at("participantId").get<std::string>();
        cfg.runId         = j.at("runId").get<std::string>();
        cfg.symbol        = j.at("symbol").get<std::string>();
        cfg.strategy      = j.at("strategy").get<std::string>();
    } catch (const nlohmann::json::out_of_range& e) {
        throw std::runtime_error(std::string("Missing required field in ") + path + ": " + e.what());
    }

    // Optional fields with defaults.
    cfg.serverUri         = j.value("serverUri",         std::string("localhost:50051"));
    cfg.randomSeed        = j.value("randomSeed",        0u);
    cfg.maxRuntimeSeconds = j.value("maxRuntimeSeconds", 300);
    cfg.shutdownTimeoutMs = j.value("shutdownTimeoutMs", 5000);
    cfg.tickSize          = j.value("tickSize",          0.01);
    cfg.decisionIntervalMs = j.value("decisionIntervalMs", 100);
    cfg.level2Depth       = j.value("level2Depth",       15);
    cfg.level2IntervalMs  = j.value("level2IntervalMs",  250);
    cfg.eventLogPath      = j.value("eventLogPath",      std::string());

    if (cfg.tickSize <= 0.0) {
        throw std::runtime_error("tickSize must be > 0 in " + path);
    }
    if (cfg.decisionIntervalMs <= 0) {
        throw std::runtime_error("decisionIntervalMs must be > 0 in " + path);
    }

    if (j.contains("parameters")) {
        cfg.parameters = j["parameters"];
    }

    return cfg;
}

ParticipantConfig ParticipantConfig::fromArgs(int argc, char** argv) {
    std::string configPath;
    std::string strategyOverride;
    std::string serverOverride;
    std::string participantIdOverride;
    std::string runIdOverride;
    int seedOverride = -1;

    for (int i = 1; i < argc; ++i) {
        std::string arg = argv[i];
        if (arg == "--config" && i + 1 < argc) {
            configPath = argv[++i];
        } else if (arg == "--strategy" && i + 1 < argc) {
            strategyOverride = argv[++i];
        } else if (arg == "--server" && i + 1 < argc) {
            serverOverride = argv[++i];
        } else if (arg == "--participant-id" && i + 1 < argc) {
            participantIdOverride = argv[++i];
        } else if (arg == "--run-id" && i + 1 < argc) {
            runIdOverride = argv[++i];
        } else if (arg == "--seed" && i + 1 < argc) {
            try {
                seedOverride = std::stoi(argv[++i]);
            } catch (...) {
                throw std::runtime_error("--seed must be an integer");
            }
        }
    }

    if (configPath.empty()) {
        throw std::runtime_error("--config <path> is required");
    }

    ParticipantConfig cfg = fromFile(configPath);

    if (!strategyOverride.empty())       cfg.strategy      = strategyOverride;
    if (!serverOverride.empty())          cfg.serverUri     = serverOverride;
    if (!participantIdOverride.empty())   cfg.participantId = participantIdOverride;
    if (!runIdOverride.empty())           cfg.runId         = runIdOverride;
    if (seedOverride >= 0)                cfg.randomSeed    = static_cast<unsigned int>(seedOverride);

    return cfg;
}
