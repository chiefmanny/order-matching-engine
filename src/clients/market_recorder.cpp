//
//  market_recorder.cpp
//
//  The experiment harness's authoritative observer.  It subscribes to the engine
//  independently of every participant and writes the raw record of what happened:
//  the trade tape, the BBO series, and periodic Level 2 snapshots.
//
//  It deliberately does NOT reuse EngineSession.  A participant's session filters
//  fills down to the orders it owns and enforces order ownership; a recorder wants
//  the complete unfiltered tape and owns nothing.  Those are different jobs, and
//  conflating them would put a "give me everything" bypass into the class whose
//  whole purpose is to restrict what a participant can see and touch.
//
//  Usage:
//    market_recorder --symbol AAPL --out-dir DIR [--server host:port]
//                    [--l2-depth 15] [--l2-interval-ms 100]
//                    [--duration-seconds N]
//
//  Runs until SIGINT/SIGTERM or --duration-seconds, then writes recorder_meta.json.
//

#include "matching_engine.grpc.pb.h"
#include <grpcpp/channel.h>
#include <grpcpp/client_context.h>
#include <grpcpp/create_channel.h>
#include <grpcpp/security/credentials.h>

#include <nlohmann/json.hpp>

#include <atomic>
#include <chrono>
#include <csignal>
#include <cstdint>
#include <fstream>
#include <iomanip>
#include <iostream>
#include <mutex>
#include <string>
#include <thread>

namespace {

std::atomic<bool> g_stop{false};
void handleSignal(int) { g_stop = true; }

int64_t nowNanos() {
    return std::chrono::duration_cast<std::chrono::nanoseconds>(
               std::chrono::system_clock::now().time_since_epoch())
        .count();
}

struct Options {
    std::string symbol      = "AAPL";
    std::string server      = "localhost:50051";
    std::string outDir      = ".";
    int l2Depth             = 15;
    int l2IntervalMs        = 100;
    int durationSeconds     = 0;  // 0 => until signalled
};

struct Counters {
    std::atomic<uint64_t> trades{0};
    std::atomic<uint64_t> bboUpdates{0};
    std::atomic<uint64_t> l2Snapshots{0};
    std::atomic<uint64_t> l2Failures{0};
    std::atomic<bool> tradeStreamEnded{false};
    std::atomic<bool> bboStreamEnded{false};
    std::string tradeStreamError;
    std::string bboStreamError;
};

// Streams hold their context so the main thread can cancel a blocked Read.
std::mutex g_contextMutex;
grpc::ClientContext* g_tradeContext = nullptr;
grpc::ClientContext* g_bboContext = nullptr;

using Stub = matchingengine::MatchingEngine::Stub;

void recordTrades(Stub* stub, const Options& opt, Counters& counters) {
    std::ofstream out(opt.outDir + "/trades.csv");
    out << "ts_ns,recv_ts_ns,symbol,price,size,aggressor_is_buy,"
           "maker_order_id,taker_order_id,partial_fill,remaining_size\n";
    out << std::fixed << std::setprecision(6);
    out.flush();  // a killed recorder must still leave a readable, headed file

    grpc::ClientContext ctx;
    {
        std::lock_guard<std::mutex> lock(g_contextMutex);
        if (g_stop) return;
        g_tradeContext = &ctx;
    }

    matchingengine::Symbol req;
    req.set_symbol(opt.symbol);
    auto reader = stub->Subscribe(&ctx, req);

    matchingengine::FillNotification f;
    while (reader->Read(&f)) {
        out << f.timestamp_ns() << ',' << nowNanos() << ',' << f.symbol() << ','
            << f.price() << ',' << f.size() << ','
            << (f.aggressor_is_buy() ? 1 : 0) << ',' << f.maker_order_id() << ','
            << f.taker_order_id() << ',' << (f.partial_fill() ? 1 : 0) << ','
            << f.remaining_size() << '\n';
        // Flush every trade: a crashed or killed run must still leave a usable tape.
        out.flush();
        counters.trades++;
    }

    grpc::Status status = reader->Finish();
    if (!status.ok() && status.error_code() != grpc::StatusCode::CANCELLED && !g_stop) {
        counters.tradeStreamError = status.error_message();
    }
    counters.tradeStreamEnded = true;

    std::lock_guard<std::mutex> lock(g_contextMutex);
    g_tradeContext = nullptr;
}

void recordBbo(Stub* stub, const Options& opt, Counters& counters) {
    std::ofstream out(opt.outDir + "/bbo.csv");
    out << "ts_ns,best_bid,bid_size,best_ask,ask_size,spread,mid\n";
    out << std::fixed << std::setprecision(6);
    out.flush();

    grpc::ClientContext ctx;
    {
        std::lock_guard<std::mutex> lock(g_contextMutex);
        if (g_stop) return;
        g_bboContext = &ctx;
    }

    matchingengine::Symbol req;
    req.set_symbol(opt.symbol);
    auto reader = stub->WatchSymbol(&ctx, req);

    matchingengine::SymbolData d;
    uint64_t sinceFlush = 0;
    while (reader->Read(&d)) {
        const double bid = d.lastbid();
        const double ask = d.lastask();
        // Spread and mid are only meaningful with both sides present; an empty side
        // reports price 0, and writing 0-derived values would poison the metrics.
        const bool twoSided = bid > 0.0 && ask > 0.0;
        out << nowNanos() << ',' << bid << ',' << d.lastbidsize() << ',' << ask << ','
            << d.lastasksize() << ',';
        if (twoSided) out << (ask - bid) << ',' << ((ask + bid) / 2.0);
        else          out << ',';   // empty fields -> NaN in pandas
        out << '\n';
        counters.bboUpdates++;
        if (++sinceFlush >= 20) { out.flush(); sinceFlush = 0; }
    }
    out.flush();

    grpc::Status status = reader->Finish();
    if (!status.ok() && status.error_code() != grpc::StatusCode::CANCELLED && !g_stop) {
        counters.bboStreamError = status.error_message();
    }
    counters.bboStreamEnded = true;

    std::lock_guard<std::mutex> lock(g_contextMutex);
    g_bboContext = nullptr;
}

void recordLevel2(Stub* stub, const Options& opt, Counters& counters) {
    // Long form (one row per level per snapshot) rather than wide: level counts and
    // prices change constantly, and long form loads straight into pandas.
    std::ofstream out(opt.outDir + "/level2.csv");
    out << "ts_ns,snapshot_id,side,rank,price,size\n";
    out << std::fixed << std::setprecision(6);
    out.flush();

    uint64_t snapshotId = 0;
    while (!g_stop) {
        const auto tickStart = std::chrono::steady_clock::now();

        grpc::ClientContext ctx;
        ctx.set_deadline(std::chrono::system_clock::now() + std::chrono::seconds(5));
        matchingengine::Level2Request req;
        req.set_symbol(opt.symbol);
        req.set_depth(opt.l2Depth);
        req.set_side(matchingengine::Level2Request::BOTH);

        auto reader = stub->GetLevel2(&ctx, req);
        matchingengine::Level2Response resp;
        const int64_t ts = nowNanos();
        bool wroteAny = false;
        while (reader->Read(&resp)) {
            for (const auto& b : resp.bids()) {
                out << ts << ',' << snapshotId << ",BID," << b.rank() << ',' << b.price()
                    << ',' << b.size() << '\n';
                wroteAny = true;
            }
            for (const auto& a : resp.asks()) {
                out << ts << ',' << snapshotId << ",ASK," << a.rank() << ',' << a.price()
                    << ',' << a.size() << '\n';
                wroteAny = true;
            }
        }
        if (reader->Finish().ok()) {
            counters.l2Snapshots++;
            // An empty book is a legitimate observation, not a gap: record the
            // snapshot id so analysis can tell "book was empty" from "recorder missed
            // a poll". A row with rank 0 and no size carries that.
            if (!wroteAny) out << ts << ',' << snapshotId << ",EMPTY,0,,\n";
            ++snapshotId;
        } else if (!g_stop) {
            counters.l2Failures++;
        }
        out.flush();

        // Sleep the remainder of the interval in short slices, so shutdown is prompt.
        const auto interval = std::chrono::milliseconds(opt.l2IntervalMs);
        while (!g_stop && std::chrono::steady_clock::now() - tickStart < interval) {
            std::this_thread::sleep_for(std::chrono::milliseconds(10));
        }
    }
    out.flush();
}

bool parseArgs(int argc, char** argv, Options& opt) {
    for (int i = 1; i < argc; ++i) {
        const std::string arg = argv[i];
        auto next = [&](const char* name) -> std::string {
            if (i + 1 >= argc) throw std::runtime_error(std::string("missing value for ") + name);
            return argv[++i];
        };
        if      (arg == "--symbol")           opt.symbol = next("--symbol");
        else if (arg == "--server")           opt.server = next("--server");
        else if (arg == "--out-dir")          opt.outDir = next("--out-dir");
        else if (arg == "--l2-depth")         opt.l2Depth = std::stoi(next("--l2-depth"));
        else if (arg == "--l2-interval-ms")   opt.l2IntervalMs = std::stoi(next("--l2-interval-ms"));
        else if (arg == "--duration-seconds") opt.durationSeconds = std::stoi(next("--duration-seconds"));
        else if (arg == "--help" || arg == "-h") {
            std::cout << "Usage: market_recorder --symbol SYM --out-dir DIR"
                         " [--server host:port] [--l2-depth N] [--l2-interval-ms N]"
                         " [--duration-seconds N]\n";
            return false;
        } else {
            std::cerr << "Unknown argument: " << arg << "\n";
            return false;
        }
    }
    return true;
}

}  // namespace

int main(int argc, char** argv) {
    Options opt;
    try {
        if (!parseArgs(argc, argv, opt)) return 1;
    } catch (const std::exception& e) {
        std::cerr << "Argument error: " << e.what() << "\n";
        return 1;
    }

    std::signal(SIGINT, handleSignal);
    std::signal(SIGTERM, handleSignal);

    auto channel = grpc::CreateChannel(opt.server, grpc::InsecureChannelCredentials());
    auto stub = matchingengine::MatchingEngine::NewStub(channel);

    // The recorder must be attached before participants start trading, so failing to
    // reach the engine is fatal rather than something to retry through.
    {
        grpc::ClientContext ctx;
        ctx.set_deadline(std::chrono::system_clock::now() + std::chrono::seconds(30));
        matchingengine::Symbol req;
        req.set_symbol(opt.symbol);
        matchingengine::SymbolData resp;
        grpc::Status status = stub->CheckSymbol(&ctx, req, &resp);
        if (!status.ok()) {
            std::cerr << "Recorder cannot reach engine at " << opt.server << " for symbol "
                      << opt.symbol << ": " << status.error_message() << "\n";
            return 2;
        }
    }

    Counters counters;
    const int64_t startedAt = nowNanos();

    std::thread tradeThread(recordTrades, stub.get(), std::cref(opt), std::ref(counters));
    std::thread bboThread(recordBbo, stub.get(), std::cref(opt), std::ref(counters));
    std::thread l2Thread(recordLevel2, stub.get(), std::cref(opt), std::ref(counters));

    std::cerr << "Recording " << opt.symbol << " to " << opt.outDir << "\n";

    const auto deadline = std::chrono::steady_clock::now() +
                          std::chrono::seconds(opt.durationSeconds > 0 ? opt.durationSeconds : 0);
    while (!g_stop) {
        if (opt.durationSeconds > 0 && std::chrono::steady_clock::now() >= deadline) break;
        std::this_thread::sleep_for(std::chrono::milliseconds(50));
    }
    g_stop = true;

    {
        std::lock_guard<std::mutex> lock(g_contextMutex);
        if (g_tradeContext) g_tradeContext->TryCancel();
        if (g_bboContext) g_bboContext->TryCancel();
    }

    if (l2Thread.joinable()) l2Thread.join();
    if (tradeThread.joinable()) tradeThread.join();
    if (bboThread.joinable()) bboThread.join();

    const int64_t endedAt = nowNanos();
    nlohmann::json meta = {
        {"symbol", opt.symbol},
        {"server", opt.server},
        {"l2_depth", opt.l2Depth},
        {"l2_interval_ms", opt.l2IntervalMs},
        {"started_ts_ns", startedAt},
        {"ended_ts_ns", endedAt},
        {"duration_seconds", (endedAt - startedAt) / 1e9},
        {"trades_recorded", counters.trades.load()},
        {"bbo_updates_recorded", counters.bboUpdates.load()},
        {"level2_snapshots_recorded", counters.l2Snapshots.load()},
        {"level2_poll_failures", counters.l2Failures.load()},
        {"trade_stream_error", counters.tradeStreamError},
        {"bbo_stream_error", counters.bboStreamError},
    };
    std::ofstream metaOut(opt.outDir + "/recorder_meta.json");
    metaOut << meta.dump(2) << "\n";

    std::cerr << "Recorded " << counters.trades.load() << " trades, "
              << counters.bboUpdates.load() << " BBO updates, "
              << counters.l2Snapshots.load() << " L2 snapshots\n";

    // A stream that died on its own (rather than being cancelled at shutdown) means
    // the recording has a hole in it, and the run should not be trusted.
    if (!counters.tradeStreamError.empty() || !counters.bboStreamError.empty()) return 3;
    return 0;
}
