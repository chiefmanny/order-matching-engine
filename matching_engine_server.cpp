//
//  matching_engine_server.cpp
//  lowlat-matching-engine
//
//  Created by Manan Manchanda on 3/31/24.
//

#include "build/matching_engine.grpc.pb.h"
#include "matching_algorithm.h"

#include <algorithm>
#include <stdio.h>
#include <map>
#include <chrono>
#include <cmath>
#include <iostream>
#include <memory>
#include <string>
#include <utility>
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
    OrderBook book;
    MatchingAlgorithm algo;

    SymbolResources(const std::string& symbol, int book_id)
        : book(symbol, book_id, askMutex, bidMutex), 
          algo(book, askMutex, bidMutex) {}
};

class MatchingEngineImpl final : public MatchingEngine::Service {
private:
    std::map<std::string, SymbolResources> resources;
    std::shared_mutex resources_mutex; // Protects access to the resources map
public:
    MatchingEngineImpl() {
        resources.emplace(std::piecewise_construct,
                  std::forward_as_tuple("GME"),     // std::forward_as_tuple: perfect forwarding
                  std::forward_as_tuple("GME", 1));
        resources.emplace(std::piecewise_construct,
                  std::forward_as_tuple("AAPL"),
                  std::forward_as_tuple("AAPL", 2));

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

        unsigned int orderId = it->second.book.addLimitOrder(price, size, buy);
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

        bool cancelled = it->second.book.cancelLimitOrder(id);

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

        auto [bid, ask] = it->second.book.getBidSizeAskSizeWithLock();

        data->set_lastask(ask.first);
        data->set_lastasksize(ask.second);
        data->set_lastbid(bid.first);
        data->set_lastbidsize(bid.second);

        return Status::OK;
    }
    
    Status WatchSymbol(ServerContext* context, const Symbol* symbol, ServerWriter<SymbolData>* writer) override {
        
    }
    
    Status Subscribe(ServerContext* context, const Symbol* symbol, ServerWriter<FillNotification>* writer) override {
        
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