#pragma once

#include <chrono>
#include <mutex>
#include <optional>
#include <string>
#include <unordered_map>
#include <vector>

enum class OrderState {
    Active,
    PartiallyFilled,
    PendingCancel,
    Canceled,
    Filled,
    Rejected
};

struct TrackedOrder {
    unsigned int engineId    = 0;
    std::string  symbol;
    bool         buy         = false;
    double       price       = 0.0;
    int          originalSize  = 0;
    int          remainingSize = 0;
    OrderState   state       = OrderState::Active;
    std::chrono::steady_clock::time_point submittedAt;
    std::chrono::steady_clock::time_point lastModifiedAt;
};

// Aggregate counters for this participant's order flow, reported at shutdown.
struct OrderTrackerStats {
    int submitted   = 0;
    int rejected    = 0;
    int canceled    = 0;
    int filled      = 0;   // orders that reached a fully-filled state
    int fillEvents  = 0;   // individual executions, including partials
    long long sharesFilled = 0;
    double meanOrderLifetimeSeconds = 0.0;  // over orders that reached a terminal state
};

// Thread-safe record of all orders placed by one participant, and the sole authority
// on what this participant owns.
//
// Fill attribution is EXACT: the engine's FillNotification carries maker_order_id and
// taker_order_id, so a fill is matched by id rather than guessed from price and side.
// (It used to guess -- pick the live order on the right side at that price with the
// smallest sufficient remaining size -- which silently mis-attributed whenever two
// participants rested at the same level, i.e. in every interesting experiment.)
class OrderTracker {
public:
    void onAccepted(unsigned int id, const std::string& symbol,
                    bool buy, double price, int size);
    void onRejected();

    // Applies an execution to the order with this id.  Returns true if the id belongs
    // to this participant and the fill was applied; false means the fill was someone
    // else's and should be ignored.
    bool onFill(unsigned int orderId, int size);

    void onCanceled(unsigned int id);
    void markPendingCancel(unsigned int id);

    // Ownership check: true only for an id this participant submitted.
    bool owns(unsigned int id) const;
    // True if we own the id AND it can still be acted on (not filled/canceled).
    bool isLiveOrder(unsigned int id) const;

    std::vector<unsigned int>       activeOrderIds() const;
    std::optional<TrackedOrder>     get(unsigned int id) const;
    int                             activeCount() const;
    int                             rejectionCount() const;
    OrderTrackerStats               stats() const;

private:
    mutable std::mutex mutex_;
    std::unordered_map<unsigned int, TrackedOrder> orders_;
    int rejectionCount_ = 0;
    int submittedCount_ = 0;
    int canceledCount_  = 0;
    int filledCount_    = 0;
    int fillEventCount_ = 0;
    long long sharesFilled_ = 0;
    double lifetimeSecondsTotal_ = 0.0;
    int terminalCount_ = 0;

    // Returns true if the order is in a state that can receive fills or cancels.
    static bool isLive(OrderState s) {
        return s == OrderState::Active || s == OrderState::PartiallyFilled ||
               s == OrderState::PendingCancel;
    }

    void recordTerminalLocked(const TrackedOrder& order);
};
