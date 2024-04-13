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
using grpc::Status;
using matchingengine::MatchingEngine;
using matchingengine::Order;
using matchingengine::OrderID;
using std::chrono::high_resolution_clock;
using std::chrono::duration_cast;
using std::chrono::milliseconds;

using namespace std;

class MatchingEngineClient {
public:
    MatchingEngineClient(const std::string& uri)
        : stub_(MatchingEngine::NewStub(grpc::CreateChannel(uri, grpc::InsecureChannelCredentials()))) {}

    unsigned int addOrder(const std::string& symbol, double price, unsigned int size, bool buy, bool limit = true) {
        Order order;
        order.set_symbol(symbol);
        order.set_price(price);
        order.set_size(size);
        order.set_buy(buy);
        order.set_limit(limit);

        OrderID orderID;
        ClientContext context;
        Status status = stub_->AddLimitOrder(&context, order, &orderID);
        if (!status.ok()) {
            std::cerr << "Failed to add order: " << status.error_message() << std::endl;
            return 0;
        }
        std::cout << "Order added with ID: " << orderID.id() << std::endl;
        return orderID.id();
    }

    bool cancelOrder(const std::string& symbol, unsigned int orderID) {
        OrderID request;
        request.set_symbol(symbol);
        request.set_id(orderID);

        OrderID response;
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
};

void measureLatency(MatchingEngineClient& client, const std::string& symbol, double price, unsigned int size, bool buy) {
    auto start = high_resolution_clock::now();
    unsigned int orderId = client.addOrder(symbol, price, size, buy);
    auto endAdd = high_resolution_clock::now();

    if (orderId != 0) {
        client.cancelOrder(symbol, orderId);
        auto endCancel = high_resolution_clock::now();

        auto addLatency = duration_cast<std::chrono::nanoseconds>(endAdd - start).count();
        auto cancelLatency = duration_cast<std::chrono::nanoseconds>(endCancel - endAdd).count();
        auto totalLatency = duration_cast<std::chrono::nanoseconds>(endCancel - start).count();

        std::cout << "Add Order Latency: " << addLatency << " ns\n";
        std::cout << "Cancel Order Latency: " << cancelLatency << " ns\n";
        std::cout << "Total Operation Latency: " << totalLatency << " ns\n";
    }
}

int main(int argc, char** argv) {
    MatchingEngineClient client("localhost:50051");

    // Example of adding and cancelling an order
    std::string symbol = "GME";
    double price = 150.0;
    unsigned int size = 10;
    bool buy = true;

    measureLatency(client, symbol, price, size, buy);

    return 0;
}