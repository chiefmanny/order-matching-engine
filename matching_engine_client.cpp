//
//  matching_engine_server.cpp
//  lowlat-matching-engine
//
//  Created by Manan Manchanda on 3/31/24.
//

#include "build/matching_engine.grpc.pb.h"

#include <chrono>
#include <iostream>
#include <memory>
#include <random>
#include <string>
#include <thread>

#include <grpc/grpc.h>
#include <grpcpp/channel.h>
#include <grpcpp/client_context.h>
#include <grpcpp/create_channel.h>
#include <grpcpp/security/credentials.h>

using grpc::Channel;
using grpc::ClientContext;
using grpc::ClientReader;
using grpc::ClientReaderWriter;
using grpc::ClientWriter;
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

using namespace std;

class MatchingEngineClient {
public:
    MatchingEngineClient(const std::string& uri)
        : stub_(matchingengine::MatchingEngine::NewStub(grpc::CreateChannel(uri, grpc::InsecureChannelCredentials()))) {}

    unsigned int addOrder(const std::string& symbol, double price, unsigned int size, bool buy, bool limit = true) {
        matchingengine::Order order = makeOrder(symbol, price, size, buy, limit);
        matchingengine::OrderID orderID;
        grpc::ClientContext context;
        grpc::Status status = stub_->AddLimitOrder(&context, order, &orderID);
        if (!status.ok()) {
            std::cerr << "Failed to add order: " << status.error_message() << std::endl;
            return 0;
        }
        std::cout << "Order added with ID: " << orderID.id() << std::endl;
        return orderID.id();
    }

    bool cancelOrder(const std::string& symbol, unsigned int orderID) {
        matchingengine::OrderID request;
        request.set_symbol(symbol);
        request.set_id(orderID);

        matchingengine::OrderID response;
        grpc::ClientContext context;
        grpc::Status status = stub_->CancelLimitOrder(&context, request, &response);
        if (!status.ok()) {
            std::cerr << "Failed to cancel order: " << status.error_message() << std::endl;
            return false;
        }
        std::cout << "Order canceled: " << response.id() << std::endl;
        return true;
    }

private:
    std::unique_ptr<matchingengine::MatchingEngine::Stub> stub_;

    matchingengine::Order makeOrder(const std::string& symbol, double price, unsigned int size, bool buy, bool limit) {
        matchingengine::Order order;
        order.set_symbol(symbol);
        order.set_price(price);
        order.set_size(size);
        order.set_buy(buy);
        order.set_limit(limit);
        return order;
    }
};

int main(int argc, char** argv) {
    MatchingEngineClient client("localhost:50051");
    unsigned int orderID = client.addOrder("GME", 100.0, 10, true);
    if (orderID != 0) {  // Check if the order was successfully added before attempting to cancel.
        client.cancelOrder("GME", orderID);
    }
    return 0;
}