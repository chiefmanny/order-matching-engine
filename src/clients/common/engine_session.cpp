#include "engine_session.h"

#include <grpcpp/security/credentials.h>

#include <utility>

namespace {
constexpr int kMaxOrderSize = 100000000;  // sanity bound; engine takes an int32 size
}

EngineSession::EngineSession(const std::string& uri,
                             std::string symbolIn,
                             OrderTracker& tracker,
                             EventLog& log)
    : symbol_(std::move(symbolIn)),
      tracker_(tracker),
      log_(log),
      channel_(grpc::CreateChannel(uri, grpc::InsecureChannelCredentials())),
      stub_(matchingengine::MatchingEngine::NewStub(channel_)) {}

EngineSession::~EngineSession() { stop(); }

EngineSession::Readiness EngineSession::waitUntilReady(std::chrono::milliseconds timeout) {
    const auto deadline = std::chrono::steady_clock::now() + timeout;
    while (std::chrono::steady_clock::now() < deadline) {
        grpc::ClientContext ctx;
        ctx.set_deadline(std::chrono::system_clock::now() + std::chrono::seconds(2));
        matchingengine::Symbol req;
        req.set_symbol(symbol_);
        matchingengine::SymbolData resp;

        grpc::Status status = stub_->CheckSymbol(&ctx, req, &resp);
        if (status.ok()) return Readiness::Ready;

        // A registered-symbol failure will never resolve by waiting.
        if (status.error_code() == grpc::StatusCode::NOT_FOUND) {
            return Readiness::SymbolNotFound;
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(100));
    }
    return Readiness::Timeout;
}

unsigned int EngineSession::submit(bool buy, double price, int size) {
    if (size <= 0 || size > kMaxOrderSize || !(price > 0.0)) {
        tracker_.onRejected();
        counters_.rpcErrors++;
        log_.rejected(buy, price, size, "invalid_order_parameters");
        return 0;
    }

    matchingengine::Order order;
    order.set_symbol(symbol_);
    order.set_price(price);
    order.set_size(size);
    order.set_buy(buy);
    order.set_limit(true);

    matchingengine::OrderID reply;
    grpc::ClientContext ctx;
    ctx.set_deadline(std::chrono::system_clock::now() + std::chrono::seconds(5));

    grpc::Status status = stub_->AddLimitOrder(&ctx, order, &reply);
    if (!status.ok()) {
        tracker_.onRejected();
        counters_.rpcErrors++;
        log_.rejected(buy, price, size, status.error_message());
        return 0;
    }

    // The engine returns id 0 to mean "rejected" (bad price or size).
    const unsigned int id = static_cast<unsigned int>(reply.id());
    if (id == 0) {
        tracker_.onRejected();
        log_.rejected(buy, price, size, "engine_rejected");
        return 0;
    }

    tracker_.onAccepted(id, symbol_, buy, price, size);
    log_.submitted(id, buy, price, size);
    return id;
}

bool EngineSession::cancel(unsigned int orderId) {
    // Ownership gate. The engine will happily cancel ANY id, including another
    // participant's, so this check is the only thing preventing cross-participant
    // interference in a multi-participant experiment.
    if (!tracker_.owns(orderId)) {
        counters_.ownershipViolations++;
        log_.emit("reject", {{"order_id", orderId},
                             {"action", "cancel"},
                             {"reason", "not_owned"}});
        return false;
    }
    if (!tracker_.isLiveOrder(orderId)) {
        // Already filled or canceled; not an error, just nothing to do.
        log_.emit("cancel", {{"order_id", orderId}, {"ok", false}, {"reason", "not_live"}});
        return false;
    }

    tracker_.markPendingCancel(orderId);

    matchingengine::OrderID req;
    req.set_symbol(symbol_);
    req.set_id(static_cast<int32_t>(orderId));
    matchingengine::OrderID reply;
    grpc::ClientContext ctx;
    ctx.set_deadline(std::chrono::system_clock::now() + std::chrono::seconds(5));

    grpc::Status status = stub_->CancelLimitOrder(&ctx, req, &reply);
    if (!status.ok()) {
        counters_.rpcErrors++;
        log_.canceled(orderId, false);
        return false;
    }

    // CancelLimitOrder returns OK even when the order was already gone (it discards
    // the book's boolean result), so we cannot distinguish those cases from the RPC.
    // Treating it as canceled is right for our bookkeeping: either it is gone, or it
    // filled and the fill stream has already moved it to a terminal state.
    tracker_.onCanceled(orderId);
    log_.canceled(orderId, true);
    return true;
}

unsigned int EngineSession::replace(unsigned int orderId, double price, int size) {
    if (!cancel(orderId)) return 0;
    auto prior = tracker_.get(orderId);
    const bool buy = prior.has_value() ? prior->buy : false;
    return submit(buy, price, size);
}

void EngineSession::startFillStream(FillCallback cb, TapeCallback tapeCb) {
    fillThread_ = std::thread(&EngineSession::runFillStream, this,
                              std::move(cb), std::move(tapeCb));
}

void EngineSession::runFillStream(FillCallback cb, TapeCallback tapeCb) {
    grpc::ClientContext ctx;
    {
        std::lock_guard<std::mutex> lock(contextMutex_);
        if (stopping_) return;
        fillContext_ = &ctx;
    }

    matchingengine::Symbol req;
    req.set_symbol(symbol_);
    auto reader = stub_->Subscribe(&ctx, req);

    matchingengine::FillNotification notif;
    while (reader->Read(&notif)) {
        counters_.fillsSeen++;
        // The tape first: every trade counts towards market volume regardless of
        // whose it was.
        if (tapeCb) tapeCb(notif.price(), notif.size(), notif.aggressor_is_buy());

        // Attribute by id. Both sides can be ours if a participant trades with
        // itself, so check each independently rather than else-if.
        const unsigned int makerId = notif.maker_order_id();
        const unsigned int takerId = notif.taker_order_id();

        for (int side = 0; side < 2; ++side) {
            const unsigned int id = side == 0 ? makerId : takerId;
            const bool isMaker = side == 0;
            if (id == 0) continue;
            if (!tracker_.owns(id)) continue;

            auto tracked = tracker_.get(id);
            if (!tracked.has_value()) continue;

            if (!tracker_.onFill(id, notif.size())) continue;

            counters_.fillsOurs++;
            Fill fill;
            fill.orderId = id;
            fill.price   = notif.price();
            fill.size    = notif.size();
            fill.buy     = tracked->buy;
            fill.isMaker = isMaker;
            fill.tsNs    = notif.timestamp_ns();

            log_.filled(id, fill.buy, fill.price, fill.size, isMaker);
            if (cb) cb(fill);
        }
    }

    grpc::Status status = reader->Finish();
    if (!status.ok() && status.error_code() != grpc::StatusCode::CANCELLED && !stopping_) {
        counters_.rpcErrors++;
        log_.emit("stream_error", {{"stream", "fills"}, {"error", status.error_message()}});
    }
    counters_.fillStreamEnded = true;

    std::lock_guard<std::mutex> lock(contextMutex_);
    fillContext_ = nullptr;
}

void EngineSession::startBboStream(BboCallback cb) {
    bboThread_ = std::thread(&EngineSession::runBboStream, this, std::move(cb));
}

void EngineSession::runBboStream(BboCallback cb) {
    grpc::ClientContext ctx;
    {
        std::lock_guard<std::mutex> lock(contextMutex_);
        if (stopping_) return;
        bboContext_ = &ctx;
    }

    matchingengine::Symbol req;
    req.set_symbol(symbol_);
    auto reader = stub_->WatchSymbol(&ctx, req);

    matchingengine::SymbolData data;
    while (reader->Read(&data)) {
        counters_.bboUpdates++;
        if (cb) {
            cb(data.lastbid(), data.lastbidsize(), data.lastask(), data.lastasksize());
        }
    }

    grpc::Status status = reader->Finish();
    if (!status.ok() && status.error_code() != grpc::StatusCode::CANCELLED && !stopping_) {
        counters_.rpcErrors++;
        log_.emit("stream_error", {{"stream", "bbo"}, {"error", status.error_message()}});
    }
    counters_.bboStreamEnded = true;

    std::lock_guard<std::mutex> lock(contextMutex_);
    bboContext_ = nullptr;
}

bool EngineSession::pollLevel2(int depth,
                               std::vector<PriceLevel>& bids,
                               std::vector<PriceLevel>& asks) {
    bids.clear();
    asks.clear();

    grpc::ClientContext ctx;
    ctx.set_deadline(std::chrono::system_clock::now() + std::chrono::seconds(5));
    matchingengine::Level2Request req;
    req.set_symbol(symbol_);
    req.set_depth(depth);
    req.set_side(matchingengine::Level2Request::BOTH);

    auto reader = stub_->GetLevel2(&ctx, req);
    matchingengine::Level2Response resp;
    while (reader->Read(&resp)) {
        for (const auto& b : resp.bids()) {
            bids.push_back(PriceLevel{b.price(), b.size(), b.rank()});
        }
        for (const auto& a : resp.asks()) {
            asks.push_back(PriceLevel{a.price(), a.size(), a.rank()});
        }
    }

    grpc::Status status = reader->Finish();
    if (!status.ok()) {
        if (!stopping_) counters_.rpcErrors++;
        return false;
    }
    counters_.level2Polls++;
    return true;
}

int EngineSession::cancelAllOwned() {
    for (unsigned int id : tracker_.activeOrderIds()) {
        cancel(id);
    }
    return tracker_.activeCount();
}

void EngineSession::stop() {
    if (stopping_.exchange(true)) return;  // idempotent

    {
        std::lock_guard<std::mutex> lock(contextMutex_);
        if (fillContext_) fillContext_->TryCancel();
        if (bboContext_) bboContext_->TryCancel();
    }

    if (fillThread_.joinable()) fillThread_.join();
    if (bboThread_.joinable()) bboThread_.join();
}
