#include "order_tracker.h"

void OrderTracker::onAccepted(unsigned int id, const std::string& symbol,
                               bool buy, double price, int size) {
    std::lock_guard<std::mutex> lock(mutex_);
    auto now = std::chrono::steady_clock::now();
    TrackedOrder order;
    order.engineId      = id;
    order.symbol        = symbol;
    order.buy           = buy;
    order.price         = price;
    order.originalSize  = size;
    order.remainingSize = size;
    order.state         = OrderState::Active;
    order.submittedAt   = now;
    order.lastModifiedAt = now;
    orders_[id] = std::move(order);
    ++submittedCount_;
}

void OrderTracker::onRejected() {
    std::lock_guard<std::mutex> lock(mutex_);
    ++rejectionCount_;
}

void OrderTracker::recordTerminalLocked(const TrackedOrder& order) {
    const double seconds = std::chrono::duration<double>(
                               order.lastModifiedAt - order.submittedAt).count();
    lifetimeSecondsTotal_ += seconds;
    ++terminalCount_;
}

bool OrderTracker::onFill(unsigned int orderId, int size) {
    if (size <= 0) return false;
    std::lock_guard<std::mutex> lock(mutex_);

    auto it = orders_.find(orderId);
    if (it == orders_.end()) return false;   // not ours
    auto& order = it->second;
    if (!isLive(order.state)) return false;  // already terminal; stale notification

    const int applied = size < order.remainingSize ? size : order.remainingSize;
    order.remainingSize -= applied;
    order.lastModifiedAt = std::chrono::steady_clock::now();

    ++fillEventCount_;
    sharesFilled_ += applied;

    if (order.remainingSize == 0) {
        order.state = OrderState::Filled;
        ++filledCount_;
        recordTerminalLocked(order);
    } else {
        order.state = OrderState::PartiallyFilled;
    }
    return true;
}

void OrderTracker::onCanceled(unsigned int id) {
    std::lock_guard<std::mutex> lock(mutex_);
    auto it = orders_.find(id);
    if (it == orders_.end()) return;
    if (!isLive(it->second.state)) return;  // already filled/canceled; leave it be
    it->second.state = OrderState::Canceled;
    it->second.lastModifiedAt = std::chrono::steady_clock::now();
    ++canceledCount_;
    recordTerminalLocked(it->second);
}

void OrderTracker::markPendingCancel(unsigned int id) {
    std::lock_guard<std::mutex> lock(mutex_);
    auto it = orders_.find(id);
    if (it == orders_.end()) return;
    if (it->second.state == OrderState::Active ||
        it->second.state == OrderState::PartiallyFilled) {
        it->second.state = OrderState::PendingCancel;
    }
}

bool OrderTracker::owns(unsigned int id) const {
    std::lock_guard<std::mutex> lock(mutex_);
    return orders_.find(id) != orders_.end();
}

bool OrderTracker::isLiveOrder(unsigned int id) const {
    std::lock_guard<std::mutex> lock(mutex_);
    auto it = orders_.find(id);
    return it != orders_.end() && isLive(it->second.state);
}

std::vector<unsigned int> OrderTracker::activeOrderIds() const {
    std::lock_guard<std::mutex> lock(mutex_);
    std::vector<unsigned int> result;
    for (const auto& entry : orders_) {
        if (isLive(entry.second.state)) {
            result.push_back(entry.first);
        }
    }
    return result;
}

std::optional<TrackedOrder> OrderTracker::get(unsigned int id) const {
    std::lock_guard<std::mutex> lock(mutex_);
    auto it = orders_.find(id);
    if (it == orders_.end()) return std::nullopt;
    return it->second;
}

int OrderTracker::activeCount() const {
    std::lock_guard<std::mutex> lock(mutex_);
    int count = 0;
    for (const auto& entry : orders_) {
        if (isLive(entry.second.state)) ++count;
    }
    return count;
}

int OrderTracker::rejectionCount() const {
    std::lock_guard<std::mutex> lock(mutex_);
    return rejectionCount_;
}

OrderTrackerStats OrderTracker::stats() const {
    std::lock_guard<std::mutex> lock(mutex_);
    OrderTrackerStats s;
    s.submitted    = submittedCount_;
    s.rejected     = rejectionCount_;
    s.canceled     = canceledCount_;
    s.filled       = filledCount_;
    s.fillEvents   = fillEventCount_;
    s.sharesFilled = sharesFilled_;
    s.meanOrderLifetimeSeconds =
        terminalCount_ > 0 ? lifetimeSecondsTotal_ / terminalCount_ : 0.0;
    return s;
}
