#include "event_log.h"

#include <chrono>
#include <utility>

EventLog::EventLog(std::string runId, std::string participantId, std::ostream& out)
    : runId_(std::move(runId)), participantId_(std::move(participantId)), out_(out) {}

int64_t EventLog::nowNanos() {
    return std::chrono::duration_cast<std::chrono::nanoseconds>(
               std::chrono::system_clock::now().time_since_epoch())
        .count();
}

void EventLog::emit(const std::string& type, nlohmann::json fields) {
    nlohmann::json line = fields.is_object() ? std::move(fields) : nlohmann::json::object();

    // Envelope keys are authoritative: a caller cannot spoof them via `fields`.
    line["ts_ns"]          = nowNanos();
    line["run_id"]         = runId_;
    line["participant_id"] = participantId_;
    line["type"]           = type;

    std::lock_guard<std::mutex> lock(mutex_);
    // dump() on one line, then an explicit flush: the harness tails this stream
    // while the process runs, and a crashed participant must not lose its last
    // events to a half-full buffer.
    out_ << line.dump() << "\n";
    out_.flush();
    ++emitted_;
}

void EventLog::submitted(unsigned int orderId, bool buy, double price, int size) {
    emit("submit", {{"order_id", orderId}, {"buy", buy}, {"price", price}, {"size", size}});
}

void EventLog::rejected(bool buy, double price, int size, const std::string& reason) {
    emit("reject", {{"buy", buy}, {"price", price}, {"size", size}, {"reason", reason}});
}

void EventLog::canceled(unsigned int orderId, bool ok) {
    emit("cancel", {{"order_id", orderId}, {"ok", ok}});
}

void EventLog::filled(unsigned int orderId, bool buy, double price, int size, bool isMaker) {
    emit("fill", {{"order_id", orderId}, {"buy", buy}, {"price", price},
                  {"size", size}, {"is_maker", isMaker}});
}

void EventLog::note(const std::string& message, nlohmann::json fields) {
    if (!fields.is_object()) fields = nlohmann::json::object();
    fields["message"] = message;
    emit("strategy_note", std::move(fields));
}

uint64_t EventLog::emittedCount() const {
    std::lock_guard<std::mutex> lock(mutex_);
    return emitted_;
}
