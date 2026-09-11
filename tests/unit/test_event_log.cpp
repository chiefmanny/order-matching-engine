#include "event_log.h"

#include <cassert>
#include <iostream>
#include <sstream>
#include <string>
#include <thread>
#include <vector>

namespace {

// Splits the log output into parsed JSON objects, one per line.
std::vector<nlohmann::json> parseLines(const std::string& text) {
    std::vector<nlohmann::json> out;
    std::istringstream in(text);
    std::string line;
    while (std::getline(in, line)) {
        if (line.empty()) continue;
        out.push_back(nlohmann::json::parse(line));  // throws if not valid JSON
    }
    return out;
}

void test_every_line_is_valid_json_with_envelope() {
    std::ostringstream out;
    EventLog log("run_007", "lp1", out);
    log.emit("submit", {{"order_id", 42}});

    auto lines = parseLines(out.str());
    assert(lines.size() == 1);
    assert(lines[0]["run_id"] == "run_007");
    assert(lines[0]["participant_id"] == "lp1");
    assert(lines[0]["type"] == "submit");
    assert(lines[0]["order_id"] == 42);
    assert(lines[0]["ts_ns"].get<int64_t>() > 0);
    std::cout << "PASS: " << __func__ << std::endl;
}

void test_one_object_per_line() {
    std::ostringstream out;
    EventLog log("r", "p", out);
    log.emit("a");
    log.emit("b");
    log.emit("c");

    // Exactly three newline-terminated records, and no stray newlines inside them.
    auto lines = parseLines(out.str());
    assert(lines.size() == 3);
    assert(lines[0]["type"] == "a");
    assert(lines[2]["type"] == "c");
    assert(log.emittedCount() == 3);
    std::cout << "PASS: " << __func__ << std::endl;
}

// The envelope identifies which participant produced a line; a strategy must not be
// able to forge it by passing the same key in its fields.
void test_envelope_keys_cannot_be_spoofed() {
    std::ostringstream out;
    EventLog log("real_run", "real_participant", out);
    log.emit("fill", {{"participant_id", "impostor"},
                      {"run_id", "fake"},
                      {"type", "not_a_fill"},
                      {"size", 10}});

    auto lines = parseLines(out.str());
    assert(lines.size() == 1);
    assert(lines[0]["participant_id"] == "real_participant");
    assert(lines[0]["run_id"] == "real_run");
    assert(lines[0]["type"] == "fill");
    assert(lines[0]["size"] == 10);  // non-reserved fields survive
    std::cout << "PASS: " << __func__ << std::endl;
}

void test_typed_helpers_emit_expected_shapes() {
    std::ostringstream out;
    EventLog log("r", "p", out);
    log.submitted(11, true, 499.5, 100);
    log.rejected(false, 501.0, 50, "size_too_large");
    log.canceled(11, true);
    log.filled(11, true, 499.5, 40, true);
    log.note("recentering", {{"from", 500.0}});

    auto lines = parseLines(out.str());
    assert(lines.size() == 5);

    assert(lines[0]["type"] == "submit" && lines[0]["order_id"] == 11 &&
           lines[0]["buy"] == true && lines[0]["size"] == 100);
    assert(lines[1]["type"] == "reject" && lines[1]["reason"] == "size_too_large");
    assert(lines[2]["type"] == "cancel" && lines[2]["ok"] == true);
    assert(lines[3]["type"] == "fill" && lines[3]["size"] == 40 &&
           lines[3]["is_maker"] == true);
    assert(lines[4]["type"] == "strategy_note" && lines[4]["message"] == "recentering" &&
           lines[4]["from"] == 500.0);
    std::cout << "PASS: " << __func__ << std::endl;
}

// The fill, market-data, and decision threads all emit concurrently.  Interleaved
// writes must never produce a torn line, or the harness cannot parse the file.
void test_concurrent_emits_do_not_tear_lines() {
    std::ostringstream out;
    EventLog log("r", "p", out);

    constexpr int kThreads = 8;
    constexpr int kPerThread = 200;
    std::vector<std::thread> threads;
    for (int t = 0; t < kThreads; ++t) {
        threads.emplace_back([&log, t] {
            for (int i = 0; i < kPerThread; ++i) {
                log.emit("tick", {{"thread", t}, {"i", i}});
            }
        });
    }
    for (auto& th : threads) th.join();

    auto lines = parseLines(out.str());  // would throw on a torn line
    assert(lines.size() == kThreads * kPerThread);
    assert(log.emittedCount() == static_cast<uint64_t>(kThreads * kPerThread));
    std::cout << "PASS: " << __func__ << std::endl;
}

void test_non_object_fields_are_tolerated() {
    std::ostringstream out;
    EventLog log("r", "p", out);
    log.emit("odd", nlohmann::json::array({1, 2, 3}));  // misuse must not corrupt output

    auto lines = parseLines(out.str());
    assert(lines.size() == 1);
    assert(lines[0]["type"] == "odd");
    std::cout << "PASS: " << __func__ << std::endl;
}

}  // namespace

void run_event_log_tests() {
    std::cout << "--- EventLog Tests ---" << std::endl;
    test_every_line_is_valid_json_with_envelope();
    test_one_object_per_line();
    test_envelope_keys_cannot_be_spoofed();
    test_typed_helpers_emit_expected_shapes();
    test_concurrent_emits_do_not_tear_lines();
    test_non_object_fields_are_tolerated();
    std::cout << "All EventLog tests passed." << std::endl;
}
