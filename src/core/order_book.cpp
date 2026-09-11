#include "order_book.h"
#include <algorithm>
#include <chrono>
#include <iostream>

using namespace std;

std::random_device OrderBook::rd;
std::mt19937 OrderBook::gen(OrderBook::rd()); // Initialize gen with rd
// Order ids are drawn from [1, INT32_MAX], NOT the full unsigned range.
//
// The wire type of OrderID.id is int32, so an id above 2^31 came back to clients as a
// negative number -- it still round-tripped for cancels (same bits) but made ids
// useless for logging, CSV artifacts and fill attribution, where they must compare
// equal to FillNotification's maker/taker ids.  ~2.1e9 ids is far more than any
// simulation needs.  0 stays reserved to mean "rejected".
std::uniform_int_distribution<unsigned int> OrderBook::distribution(1, 2147483647u);
std::atomic<bool> OrderBook::verbose{false};

OrderBook::OrderBook(std::string security_in, unsigned int securityId_in, std::shared_mutex& askMutex_in, std::shared_mutex& bidMutex_in) : security(security_in), securityId(securityId_in), askMutex(askMutex_in), bidMutex(bidMutex_in) {
    ask = nullptr;
    bid = nullptr;
    volume = 0;
}

unsigned int OrderBook::addLimitOrder(double price_in, unsigned int size_in, bool buy) {
    if (price_in <= 0.0 || size_in == 0) return 0; // order_id of 0 implies unsuccessful operation
    unsigned int id = generateOrderId();

    // Arrival order decides maker vs taker when this order later matches.
    const uint64_t seq = arrivalSeq_.fetch_add(1) + 1;

    std::shared_ptr<OrderInternal> addOrder = std::make_shared<OrderInternal>(id, size_in, OrderType::Limit, price_in, nullptr, nullptr, seq); // pointers unneeded for FIFO

    if (buy) {
        std::scoped_lock lockTreeAndHash(bidMutex, buyOrdersMutex);
        auto& limit = this->bidTree[price_in];
        limit.orders.push_back(addOrder);
        limit.totalSize += addOrder->size;
        limit.numOrders += 1;
        allBuyOrders[id] = addOrder;
    } else {
        std::scoped_lock lockTreeAndHash(askMutex, sellOrdersMutex);
        auto& limit = this->askTree[price_in];
        limit.orders.push_back(addOrder);
        limit.totalSize += addOrder->size;
        limit.numOrders += 1;
        allSellOrders[id] = addOrder;
    }
    return id;
}


bool OrderBook::cancelLimitOrder(unsigned int orderId_in) { // could ask for more info from client to reduce server side overhead
    {
        std::scoped_lock lockTreeAndHash(bidMutex, buyOrdersMutex);
        auto buyIt = allBuyOrders.find(orderId_in);
        if (buyIt != allBuyOrders.end()) {
            const double price = buyIt->second->price;
            // find(), never operator[]: an id whose level is already gone (fully
            // matched) must not resurrect an empty level and underflow its totalSize.
            auto limitIt = bidTree.find(price);
            if (limitIt == bidTree.end()) {
                allBuyOrders.erase(buyIt);
                return false;
            }
            auto& limit = limitIt->second;
            const auto before = limit.orders.size();
            limit.orders.erase(std::remove(limit.orders.begin(), limit.orders.end(), buyIt->second), limit.orders.end());
            if (limit.orders.size() == before) {
                // Order is indexed but no longer resting at this level.
                allBuyOrders.erase(buyIt);
                return false;
            }
            limit.totalSize -= buyIt->second->size;
            limit.numOrders -= 1;
            allBuyOrders.erase(buyIt);
            if (limit.orders.empty()) {
                if (verbose) std::cout << "Thread ID" << std::this_thread::get_id() << " Old Bid: " << price << std::endl;
                bidTree.erase(price);
                refreshBidPointer(); // refreshBid
                if (verbose) std::cout << "Thread ID" << std::this_thread::get_id() << " New Bid: " << getBid() << std::endl;
            }
            return true;
        }
    }
    {
        std::scoped_lock lockTreeAndHash(askMutex, sellOrdersMutex);
        auto sellIt = allSellOrders.find(orderId_in);
        if (sellIt != allSellOrders.end()) {
            const double price = sellIt->second->price;
            auto limitIt = askTree.find(price);
            if (limitIt == askTree.end()) {
                allSellOrders.erase(sellIt);
                return false;
            }
            auto& limit = limitIt->second;
            const auto before = limit.orders.size();
            limit.orders.erase(std::remove(limit.orders.begin(), limit.orders.end(), sellIt->second), limit.orders.end());
            if (limit.orders.size() == before) {
                allSellOrders.erase(sellIt);
                return false;
            }
            limit.totalSize -= sellIt->second->size;
            limit.numOrders -= 1;
            allSellOrders.erase(sellIt);
            if (limit.orders.empty()) {
                if (verbose) std::cout << "Thread ID" << std::this_thread::get_id() << " Old Ask: " << price << std::endl;
                askTree.erase(price);
                refreshAskPointer(); // refreshAsk
                if (verbose) std::cout << "Thread ID" << std::this_thread::get_id() << " New Ask: " << getAsk() << std::endl;
            }
            return true;
        }
    }
    return false;
}

unsigned int OrderBook::generateOrderId() {
    std::shared_lock<std::shared_mutex> lockBuyHash(buyOrdersMutex);
    std::shared_lock<std::shared_mutex> lockSellHash(sellOrdersMutex);
    unsigned int randomNumber;
    do {
        randomNumber = distribution(gen);
    } while (randomNumber == 0 || allBuyOrders.find(randomNumber) != allBuyOrders.end() || allSellOrders.find(randomNumber) != allSellOrders.end());

    return randomNumber;
}

const double OrderBook::getAsk() {
    if (!askEmpty()) {
        return askTree.begin()->first;
    }
    else {
        return 0;
    }
        
}

const double OrderBook::getBid() {
    if (!bidEmpty()) {
        return bidTree.begin()->first;
    }
    else {
        return 0;
    }
}

const double OrderBook::getAskWithLock() {
    std::shared_lock<std::shared_mutex> lockAsks(askMutex);
    return getAsk();
}

const double OrderBook::getBidWithLock() {
    std::shared_lock<std::shared_mutex> lockBids(bidMutex);
    return getBid();
}

const std::pair<std::pair<double, unsigned int>, std::pair<double, unsigned int>> OrderBook::getBidSizeAskSizeWithLock() {
    std::shared_lock<std::shared_mutex> lockAsks(askMutex);
    std::shared_lock<std::shared_mutex> lockBids(bidMutex);

    if (askTree.empty() && bidTree.empty()) {
        return {{0.0, 0}, {0.0, 0}};  // Handle empty order books.
    }

    std::pair<double, unsigned int> askInfo = {0.0, 0};
    std::pair<double, unsigned int> bidInfo = {0.0, 0};

    // Report AGGREGATE size at the touch, matching what GetLevel2 reports for rank 1.
    // This previously returned only the front-of-queue order's size, so the BBO feed
    // and the Level 2 feed disagreed about the same price level.
    if (!askTree.empty()) {
        auto askIt = askTree.begin();
        askInfo.first  = askIt->first;
        askInfo.second = askIt->second.totalSize;
    }

    if (!bidTree.empty()) {
        auto bidIt = bidTree.begin();
        bidInfo.first  = bidIt->first;
        bidInfo.second = bidIt->second.totalSize;
    }

    return {askInfo, bidInfo};
}

double OrderBook::refreshAskPointer() {
    if (!askEmpty()) {
        ask = &(askTree.begin()->second);
        return askTree.begin()->first;
    }
    else {
        ask = nullptr;
        return 0;
    }
}

double OrderBook::refreshBidPointer() {
    if (!bidEmpty()) {
        bid = &(bidTree.begin()->second);
        return bidTree.begin()->first;
    }
    else {
        bid = nullptr;
        return 0;
    }
}

bool OrderBook::askEmpty() {
    return askTree.empty();
}

bool OrderBook::bidEmpty() {
    return bidTree.empty();
}

std::vector<std::pair<double, unsigned int>> OrderBook::getTopBidsWithLock(int depth) {
    std::vector<std::pair<double, unsigned int>> topBids;
    std::shared_lock<std::shared_mutex> lock(bidMutex);

    for (auto it = bidTree.begin(); it != bidTree.end() && topBids.size() < static_cast<size_t>(depth); ++it) {
        topBids.emplace_back(it->first, it->second.totalSize);
    }

    return topBids;
}

std::vector<std::pair<double, unsigned int>> OrderBook::getTopAsksWithLock(int depth) {
    std::vector<std::pair<double, unsigned int>> topAsks;
    std::shared_lock<std::shared_mutex> lock(askMutex);

    for (auto it = askTree.begin(); it != askTree.end() && topAsks.size() < static_cast<size_t>(depth); ++it) {
        topAsks.emplace_back(it->first, it->second.totalSize);
    }

    return topAsks;
}

unsigned int OrderBook::totalRestingSize(bool buy) {
    unsigned int sum = 0;
    if (buy) {
        std::shared_lock<std::shared_mutex> lock(bidMutex);
        for (const auto& [price, limit] : bidTree) {
            for (const auto& order : limit.orders) sum += order->size;
        }
    } else {
        std::shared_lock<std::shared_mutex> lock(askMutex);
        for (const auto& [price, limit] : askTree) {
            for (const auto& order : limit.orders) sum += order->size;
        }
    }
    return sum;
}

// ---------------------------------------------------------------------------
// Trade-tape fan-out
// ---------------------------------------------------------------------------

bool OrderBook::FillSubscriber::waitAndDrain(std::vector<FillRecord>& out,
                                             std::chrono::milliseconds timeout) {
    std::unique_lock<std::mutex> lock(mutex);
    cv.wait_for(lock, timeout, [&] { return !queue.empty() || closed; });

    while (!queue.empty()) {
        out.push_back(queue.front());
        queue.pop_front();
    }
    return !closed || !out.empty();
}

std::shared_ptr<OrderBook::FillSubscriber> OrderBook::subscribeFills(size_t maxQueued) {
    auto sub = std::make_shared<FillSubscriber>(maxQueued);
    std::unique_lock<std::shared_mutex> lock(fillSubscribersMutex_);
    fillSubscribers_.push_back(sub);
    return sub;
}

void OrderBook::unsubscribeFills(const std::shared_ptr<FillSubscriber>& sub) {
    if (!sub) return;
    {
        std::unique_lock<std::shared_mutex> lock(fillSubscribersMutex_);
        fillSubscribers_.erase(
            std::remove(fillSubscribers_.begin(), fillSubscribers_.end(), sub),
            fillSubscribers_.end());
    }
    {
        std::lock_guard<std::mutex> lock(sub->mutex);
        sub->closed = true;
    }
    sub->cv.notify_all();
}

void OrderBook::publishFill(const FillRecord& rec) {
    std::shared_lock<std::shared_mutex> lock(fillSubscribersMutex_);
    for (const auto& sub : fillSubscribers_) {
        {
            std::lock_guard<std::mutex> qLock(sub->mutex);
            if (sub->closed) continue;
            // Never block the matching thread: drop the oldest and count it.
            while (sub->queue.size() >= sub->maxQueued) {
                sub->queue.pop_front();
                ++sub->dropped;
            }
            sub->queue.push_back(rec);
        }
        sub->cv.notify_one();
    }
}

size_t OrderBook::fillSubscriberCount() {
    std::shared_lock<std::shared_mutex> lock(fillSubscribersMutex_);
    return fillSubscribers_.size();
}
