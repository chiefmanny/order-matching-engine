#ifndef order_book_h
#define order_book_h

#include "limit_price.h"

#include <atomic>
#include <cstdint>
#include <deque>
#include <map>
#include <unordered_map>
#include <queue>
#include <string>
#include <random>
#include <memory>
#include <thread>
#include <mutex>
#include <condition_variable>
#include <shared_mutex>
#include <utility>
#include <vector>

using namespace std;

class OrderBook {
public:
    const std::string security;
    const unsigned int securityId;
    unsigned int volume;

    // One executed trade.
    //
    // maker/taker are resolved by arrival sequence (OrderInternal::seq): the order
    // that arrived first is resting and is the maker, so `price` is always the
    // maker's limit price -- standard price-time priority.  `aggressor_is_buy` says
    // which side lifted/hit, which is what a trade tape needs to sign volume.
    //
    // `buy` and `partial_fill`/`remaining_size` are retained with their original
    // meanings for wire compatibility with existing subscribers: `buy` is true when
    // the BID order was the one fully consumed by this match.
    struct FillRecord {
        int64_t  ts_ns           = 0;
        double   price           = 0.0;
        uint32_t size            = 0;
        bool     buy             = false;
        bool     partial_fill    = false;
        uint32_t remaining_size  = 0;
        uint32_t maker_order_id  = 0;
        uint32_t taker_order_id  = 0;
        bool     aggressor_is_buy = false;
    };

    // A single consumer of the trade tape.
    //
    // Every subscriber gets its own bounded queue and the matching thread fans each
    // fill out to all of them, so a recorder and any number of participants each see
    // the COMPLETE tape.  (The previous design had one shared queue that subscribers
    // popped from destructively, which silently split the tape between them.)
    //
    // Overflow drops the oldest record and increments `dropped` rather than blocking
    // the matching thread; consumers report the count so a run can be marked lossy.
    struct FillSubscriber {
        explicit FillSubscriber(size_t maxQueued_in) : maxQueued(maxQueued_in) {}

        std::mutex             mutex;
        std::condition_variable cv;
        std::deque<FillRecord> queue;
        size_t                 maxQueued;
        uint64_t               dropped = 0;
        bool                   closed  = false;

        // Waits up to `timeout` for records, then moves everything queued into `out`.
        // Returns false once the subscription has been closed and drained.
        bool waitAndDrain(std::vector<FillRecord>& out, std::chrono::milliseconds timeout);
    };

    std::shared_ptr<FillSubscriber> subscribeFills(size_t maxQueued = 65536);
    void unsubscribeFills(const std::shared_ptr<FillSubscriber>& sub);
    void publishFill(const FillRecord& rec);
    size_t fillSubscriberCount();

    std::map<double, LimitPrice> askTree;
    std::map<double, LimitPrice, std::greater<double>> bidTree;
    std::unordered_map<unsigned int, std::shared_ptr<OrderInternal>> allBuyOrders;
    std::unordered_map<unsigned int, std::shared_ptr<OrderInternal>> allSellOrders;
    LimitPrice* ask;
    LimitPrice* bid;
    std::shared_mutex& askMutex;
    std::shared_mutex& bidMutex;
    std::shared_mutex buyOrdersMutex;
    std::shared_mutex sellOrdersMutex;

    OrderBook(std::string security_in, unsigned int securityId_in, std::shared_mutex& askMutex_in, std::shared_mutex& bidMutex_in);
    unsigned int addLimitOrder(double price_in, unsigned int size_in, bool buy); // returns an order_id
    bool modifyLimitOrder(double price_in, unsigned int size_in, unsigned int orderId_in);
    bool cancelLimitOrder(unsigned int orderId_in);
    unsigned int generateOrderId();
    double refreshAskPointer();
    double refreshBidPointer();
    const double getAskWithLock();
    const double getBidWithLock();
    const double getAsk();
    const double getBid();
    const std::pair<std::pair<double, unsigned int>, std::pair<double, unsigned int>> getBidSizeAskSizeWithLock();
    bool askEmpty();
    bool bidEmpty();
    std::vector<std::pair<double, unsigned int>> getTopBidsWithLock(int depth);
    std::vector<std::pair<double, unsigned int>> getTopAsksWithLock(int depth);

    // Sum of remaining sizes of every resting order on a side.  Used by tests to
    // assert the LimitPrice::totalSize bookkeeping invariant.
    unsigned int totalRestingSize(bool buy);

    // Per-book chatter on stdout.  Off by default: the matching loop used to print on
    // every match and every pointer refresh, which dominates runtime and pollutes the
    // structured logs the experiment harness collects.
    static std::atomic<bool> verbose;

    static std::random_device rd;
    static std::mt19937 gen;
    static std::uniform_int_distribution<unsigned int> distribution;

    friend class MatchingAlgorithm;

private:
    std::atomic<uint64_t> arrivalSeq_{0};
    std::shared_mutex fillSubscribersMutex_;
    std::vector<std::shared_ptr<FillSubscriber>> fillSubscribers_;
};

#endif /* order_book_h */
