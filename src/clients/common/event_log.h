#pragma once

#include <nlohmann/json.hpp>

#include <cstdint>
#include <mutex>
#include <ostream>
#include <string>

// Structured participant output: one JSON object per line.
//
// The experiment harness reads this and NOTHING else from a participant's stdout --
// human-readable chatter goes to stderr.  Every line carries the run id, participant
// id, and a wall-clock nanosecond timestamp so events from several processes can be
// merged and aligned against the recorder's trades/bbo/level2 artifacts.
//
// Thread-safe: the fill, market-data, and decision threads all emit concurrently.
class EventLog {
public:
    // `out` must outlive the log.  Typically std::cout, or a file stream.
    EventLog(std::string runId, std::string participantId, std::ostream& out);

    // Emits one line: {ts_ns, run_id, participant_id, type, ...fields}.
    // Keys in `fields` never overwrite the reserved envelope keys.
    void emit(const std::string& type, nlohmann::json fields = nlohmann::json::object());

    // Convenience wrappers for the event types the harness's metrics rely on.
    void submitted(unsigned int orderId, bool buy, double price, int size);
    void rejected(bool buy, double price, int size, const std::string& reason);
    void canceled(unsigned int orderId, bool ok);
    void filled(unsigned int orderId, bool buy, double price, int size, bool isMaker);
    void note(const std::string& message, nlohmann::json fields = nlohmann::json::object());

    uint64_t emittedCount() const;

    static int64_t nowNanos();

private:
    std::string runId_;
    std::string participantId_;
    std::ostream& out_;
    mutable std::mutex mutex_;
    uint64_t emitted_ = 0;
};
