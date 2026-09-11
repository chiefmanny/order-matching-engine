#include "participant_config.h"

#include <cassert>
#include <fstream>
#include <iostream>
#include <stdexcept>
#include <string>

static std::string write_temp_config(const std::string& json) {
    std::string path = "/tmp/test_participant_config.json";
    std::ofstream f(path);
    f << json;
    return path;
}

static void test_fromFile_loads_all_fields() {
    auto path = write_temp_config(R"({
        "participantId": "test-001",
        "runId": "run-001",
        "symbol": "AAPL",
        "strategy": "liquidity_sweep",
        "serverUri": "localhost:50052",
        "randomSeed": 42,
        "maxRuntimeSeconds": 120,
        "shutdownTimeoutMs": 3000,
        "parameters": { "side": "BUY", "targetQuantity": 1000 }
    })");

    ParticipantConfig cfg = ParticipantConfig::fromFile(path);
    assert(cfg.participantId == "test-001");
    assert(cfg.runId         == "run-001");
    assert(cfg.symbol        == "AAPL");
    assert(cfg.strategy      == "liquidity_sweep");
    assert(cfg.serverUri     == "localhost:50052");
    assert(cfg.randomSeed    == 42u);
    assert(cfg.maxRuntimeSeconds == 120);
    assert(cfg.shutdownTimeoutMs == 3000);
    assert(cfg.parameters["side"]           == "BUY");
    assert(cfg.parameters["targetQuantity"] == 1000);
    std::cout << "PASS: " << __func__ << std::endl;
}

static void test_fromFile_default_values() {
    auto path = write_temp_config(R"({
        "participantId": "p1",
        "runId": "r1",
        "symbol": "GME",
        "strategy": "noise_trader"
    })");

    ParticipantConfig cfg = ParticipantConfig::fromFile(path);
    assert(cfg.serverUri         == "localhost:50051");
    assert(cfg.randomSeed        == 0u);
    assert(cfg.maxRuntimeSeconds == 300);
    assert(cfg.shutdownTimeoutMs == 5000);
    assert(cfg.parameters.is_object());
    assert(cfg.parameters.empty());
    std::cout << "PASS: " << __func__ << std::endl;
}

static void test_fromFile_missing_required_field_throws() {
    auto path = write_temp_config(R"({
        "participantId": "p1",
        "symbol": "GME",
        "strategy": "noise_trader"
    })");
    // Missing "runId"
    bool threw = false;
    try {
        ParticipantConfig::fromFile(path);
    } catch (const std::exception&) {
        threw = true;
    }
    assert(threw);
    std::cout << "PASS: " << __func__ << std::endl;
}

static void test_fromFile_nonexistent_file_throws() {
    bool threw = false;
    try {
        ParticipantConfig::fromFile("/tmp/no_such_file_xyz_abc.json");
    } catch (const std::exception&) {
        threw = true;
    }
    assert(threw);
    std::cout << "PASS: " << __func__ << std::endl;
}

static void test_fromFile_invalid_json_throws() {
    auto path = write_temp_config("{ this is not valid json ]]]");
    bool threw = false;
    try {
        ParticipantConfig::fromFile(path);
    } catch (const std::exception&) {
        threw = true;
    }
    assert(threw);
    std::cout << "PASS: " << __func__ << std::endl;
}

void run_participant_config_tests() {
    std::cout << "--- ParticipantConfig Tests ---" << std::endl;
    test_fromFile_loads_all_fields();
    test_fromFile_default_values();
    test_fromFile_missing_required_field_throws();
    test_fromFile_nonexistent_file_throws();
    test_fromFile_invalid_json_throws();
    std::cout << "All ParticipantConfig tests passed." << std::endl;
}
