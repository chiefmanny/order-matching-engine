//
//  matching_engine_server.cpp
//  lowlat-matching-engine
//
//  Created by Manan Manchanda on 3/31/24.
//

#include "../../build/matching_engine.grpc.pb.h"
#include "../core/matching_algorithm.h"

#include <algorithm>
#include <stdio.h>
#include <map>
#include <chrono>
#include <cmath>
#include <iostream>
#include <memory>
#include <string>
#include <utility>
#include <thread>
#include <atomic>
#include <grpc/grpc.h>
#include <grpcpp/security/server_credentials.h>
#include <grpcpp/server.h>
#include <grpcpp/server_builder.h>
#include <grpcpp/server_context.h>

using grpc::Server;
using grpc::ServerBuilder;
using grpc::ServerContext;
using grpc::ServerReader;
using grpc::ServerReaderWriter;
using grpc::ServerWriter;
using grpc::Status;
using matchingengine::MatchingEngine;
using matchingengine::Order;
using matchingengine::OrderID;
using matchingengine::FillNotification;
using matchingengine::RejectedAddOrder;
using matchingengine::RejectedCancelOrder;
using matchingengine::Symbol;
using matchingengine::SymbolData;
using std::chrono::system_clock;

struct SymbolResources {
    std::shared_mutex askMutex;
    std::shared_mutex bidMutex;
    OrderBook orderbook;
    MatchingAlgorithm algo;
    std::thread algoThread;
    std::atomic<bool> running{true};

    SymbolResources(const std::string& symbol, int orderbook_id)
        : orderbook(symbol, orderbook_id, askMutex, bidMutex), 
          algo(orderbook, askMutex, bidMutex) {
          algoThread = std::thread(&SymbolResources::runAlgorithm, this);
    }
    ~SymbolResources() {
        std::thread::id this_id = std::this_thread::get_id();
        std::cout << "Thread ID " << this_id << ": Stopping " << orderbook.security << " matching algorithm..." << std::endl;
        running = false;
        if (algoThread.joinable()) {
            algoThread.join();
        }
    }

    void runAlgorithm() {
        std::thread::id this_id = std::this_thread::get_id();
        std::cout << "Thread ID " << this_id << ": Launching " << orderbook.security << " matching algorithm..." << std::endl;
        algo.run(running, false, false);
    }
};

class MatchingEngineImpl final : public MatchingEngine::Service {
private:
    std::map<std::string, SymbolResources> resources;
    std::shared_mutex resources_mutex; // Protects access to the resources map
public:
    MatchingEngineImpl() {
        resources.emplace(std::piecewise_construct,
                  std::forward_as_tuple("AAPL"),     // std::forward_as_tuple: perfect forwarding
                  std::forward_as_tuple("AAPL", 1));
        // resources.emplace(std::piecewise_construct,
        //           std::forward_as_tuple("GME"),
        //           std::forward_as_tuple("GME", 2));

        // Add more symbols as needed
    }

    Status AddLimitOrder(ServerContext* context, const Order* order, OrderID* id) override {
        // Extract order details from the client-sent Order message
        std::string symbol = order->symbol();
        double price = order->price();
        unsigned int size = order->size();
        bool buy = order->buy();
        OrderType type = order->limit() ? OrderType::Limit : OrderType::Market; // not used, all will be limit rn

        std::shared_lock<std::shared_mutex> lock(resources_mutex);
        auto it = resources.find(symbol);
        if (it == resources.end()) {
            return grpc::Status(grpc::StatusCode::NOT_FOUND, "Symbol not supported");
        }

        unsigned int orderId = it->second.orderbook.addLimitOrder(price, size, buy);
        id->set_symbol(symbol);
        id->set_id(orderId);

        return Status::OK;
    }
    
    Status CancelLimitOrder(ServerContext* context, const OrderID* id_in, OrderID* id_out) override {
        // Extract orderID from the client-sent OrderID message
        std::string symbol = id_in->symbol();
        unsigned int id = id_in->id();

        std::shared_lock<std::shared_mutex> lock(resources_mutex);
        auto it = resources.find(symbol);
        if (it == resources.end()) {
            return grpc::Status(grpc::StatusCode::NOT_FOUND, "Symbol not supported");
        }

        bool cancelled = it->second.orderbook.cancelLimitOrder(id);

        id_out->set_id(id);

        return Status::OK;
    }
    
    Status CheckSymbol(ServerContext* context, const Symbol* symbol, SymbolData* data) override {
        // Extract order details from the client-sent Symbol message
        std::string symbolToWatch = symbol->symbol();

        std::shared_lock<std::shared_mutex> lock(resources_mutex);
        auto it = resources.find(symbolToWatch);
        if (it == resources.end()) {
            return grpc::Status(grpc::StatusCode::NOT_FOUND, "Symbol not supported");
        }

        auto [ask, bid] = it->second.orderbook.getBidSizeAskSizeWithLock();

        data->set_lastask(ask.first);
        data->set_lastasksize(ask.second);
        data->set_lastbid(bid.first);
        data->set_lastbidsize(bid.second);

        return Status::OK;
    }
    
    Status WatchSymbol(ServerContext* context, const Symbol* symbol, ServerWriter<SymbolData>* writer) override {
         // Extract order details from the client-sent Symbol message
        std::string symbolToWatch = symbol->symbol();

        std::shared_lock<std::shared_mutex> lock(resources_mutex);
        auto it = resources.find(symbolToWatch);
        if (it == resources.end()) {
            return grpc::Status(grpc::StatusCode::NOT_FOUND, "Symbol not supported");
        }

        while (true) {
            // Check if the context has been canceled (e.g., client disconnected)
            if (context->IsCancelled()) {
                return grpc::Status::CANCELLED;
            }

            // Create a new SymbolData message
            SymbolData data;

            // Fetch the latest bid and ask data
            auto [ask, bid] = it->second.orderbook.getBidSizeAskSizeWithLock();

            data.set_lastask(ask.first);
            data.set_lastasksize(ask.second);
            data.set_lastbid(bid.first);
            data.set_lastbidsize(bid.second);

            if (!writer->Write(data)) {
                // If Write fails, the client likely disconnected or canceled
                return grpc::Status::CANCELLED;
            }

            std::this_thread::sleep_for(std::chrono::milliseconds(100));
        }

        // This line will never be reached, but it's good practice to include a return statement
        return grpc::Status::OK;
    }
    
    Status Subscribe(ServerContext* context, const Symbol* symbol, ServerWriter<FillNotification>* writer) override {
        std::string symbolToWatch = symbol->symbol();

        std::shared_lock<std::shared_mutex> lock(resources_mutex);
        auto it = resources.find(symbolToWatch);
        if (it == resources.end())
            return grpc::Status(grpc::StatusCode::NOT_FOUND, "Symbol not supported");

        OrderBook& orderbook = it->second.orderbook;

        while (true) {
            if (context->IsCancelled())
                return grpc::Status::CANCELLED;

            std::unique_lock<std::mutex> qLock(orderbook.fillQueueMutex);
            orderbook.fillQueueCv.wait_for(qLock, std::chrono::milliseconds(200),
                [&]{ return !orderbook.fillQueue.empty(); });

            while (!orderbook.fillQueue.empty()) {
                OrderBook::FillRecord rec = orderbook.fillQueue.front();
                orderbook.fillQueue.pop();
                qLock.unlock();

                FillNotification notif;
                notif.set_symbol(symbolToWatch);
                notif.set_price(rec.price);
                notif.set_size(rec.size);
                notif.set_buy(rec.buy);
                notif.set_partial_fill(rec.partial_fill);
                notif.set_remaining_size(rec.remaining_size);

                if (!writer->Write(notif))
                    return grpc::Status::CANCELLED;

                qLock.lock();
            }
        }
        return grpc::Status::OK;
    }

    Status GetLevel2(ServerContext* context, const matchingengine::Level2Request* request, ServerWriter<matchingengine::Level2Response>* writer) override {
        // Extract the symbol and request parameters from the client
        std::string symbol = request->symbol();
        int depth = request->depth();
        matchingengine::Level2Request::Side side = request->side();

        // Acquire a shared lock on the resources
        std::shared_lock<std::shared_mutex> lock(resources_mutex);
        auto it = resources.find(symbol);
        if (it == resources.end()) {
            return grpc::Status(grpc::StatusCode::NOT_FOUND, "Symbol not supported");
        }

        // Fetch the order book for the symbol
        auto& orderbook = it->second.orderbook;

        // Create a response object
        matchingengine::Level2Response response;

        // Fetch bids if requested
        if (side == matchingengine::Level2Request::BID || side == matchingengine::Level2Request::BOTH) {
            auto bidLevels = orderbook.getTopBidsWithLock(depth);  // Assume this function returns a vector of price levels

            for (int i = 0; i < bidLevels.size(); ++i) {
                const auto& [price, totalSize] = bidLevels[i];
                auto* bidLevel = response.add_bids();  // Add a new PriceLevelData entry
                bidLevel->set_price(price);
                bidLevel->set_size(totalSize);
                bidLevel->set_rank(i + 1);  // Rank starts at 1 for the best bid
            }
        }

        // Fetch asks if requested
        if (side == matchingengine::Level2Request::ASK || side == matchingengine::Level2Request::BOTH) {
            auto askLevels = orderbook.getTopAsksWithLock(depth);  // Assume this function returns a vector of price levels

            for (int i = 0; i < askLevels.size(); ++i) {
                const auto& [price, totalSize] = askLevels[i];
                auto* askLevel = response.add_asks();  // Add a new PriceLevelData entry
                askLevel->set_price(price);
                askLevel->set_size(totalSize);
                askLevel->set_rank(i + 1);  // Rank starts at 1 for the best ask
            }
        }

        // Send the response back to the client using the writer
        if (!writer->Write(response)) {
            return grpc::Status::CANCELLED;  // Client likely disconnected
        }

        return grpc::Status::OK;
    }
};

void RunServer() {
    std::string server_address("0.0.0.0:50051");
    MatchingEngineImpl service;

    ServerBuilder builder;
    builder.AddListeningPort(server_address, grpc::InsecureServerCredentials());
    builder.RegisterService(&service);
    std::unique_ptr<Server> server(builder.BuildAndStart());
    std::cout << "Server listening on " << server_address << std::endl;
    server->Wait();
}

int main(int argc, char** argv) {
    RunServer();
    return 0;
}