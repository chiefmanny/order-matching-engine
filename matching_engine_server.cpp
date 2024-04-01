//
//  matching_engine_server.cpp
//  lowlat-matching-engine
//
//  Created by Manan Manchanda on 3/31/24.
//

#include <stdio.h>

#include "matching_engine.grpc.pb.h"
#include "matching_algorithm.h"

#include <algorithm>
#include <chrono>
#include <cmath>
#include <iostream>
#include <memory>
#include <string>
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

class MatchingEngineImpl final : public MatchingEngine::Service {
public:
    MatchingEngineImpl() : ask(), bid(), book("GME", 1, ask, bid), algo(book, ask, bid) {}

    Status AddLimitOrder(ServerContext* context, const Order* order, OrderID* id) override {
        try {
            // Extract order details from the client-sent Order message
            std::string symbol = order->symbol(); // not used, only one symbol supported rn
            double price = order->price();
            unsigned int size = order->size();
            bool buy = order->buy();
            OrderType type = order->limit() ? OrderType:::Limit : OrderType::Market; // not used, all will be limit rn

            id->set_symbol(symbol);
            id->set_id(book.addLimitOrder(price, size, buy));

            return Status::OK
        }
        catch {
            id->set_id(0x00000000);
            return Status::NOT_FOUND;
        }
    }
    
    Status CancelLimitOrder(ServerContext* context, const OrderID* id_in, OrderID* id_out) override {
        unsigned int id = id_in->id();

        bool cancelled = book.cancelLimitOrder(id);

        id_out->set_id(cancelled ? id : 0x00000000);

        return cancelled ? Status::OK : Status::NOT_FOUND;
    }
    
    Status CheckSymbol(ServerContext* context, const Symbol* symbol, SymbolData* data) override {
        
    }
    
    Status WatchSymbol(ServerContext* context, const Symbol* symbol, ServerWriter<SymbolData>* writer) override {
        
    }
    
    Status Subscribe(ServerContext* context, const Symbol* symbol, ServerWriter<FillNotification>* writer) override {
        
    }
private:
    std::shared_mutex ask;
    std::shared_mutex bid;
    OrderBook book;
    MatchingAlgorithm algo;
};

void RunServer() {
    std::string server_address("0.0.0.0:50051");
    MatchingEngineImpl service();

    ServerBuilder builder;
    builder.AddListeningPort(server_address, grpc::InsecureServerCredentials());
    std::unique_ptr<Server> server(builder.BuildAndStart());
    std::cout << "Server listening on " << server_address << std::endl;
    server->Wait();
}

int main(int argc, char** argv) {
    RunServer();
    return 0;
}