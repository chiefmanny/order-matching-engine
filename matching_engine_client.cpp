//
//  matching_engine_server.cpp
//  lowlat-matching-engine
//
//  Created by Manan Manchanda on 3/31/24.
//

#include "build/matching_engine.grpc.pb.h"
#include "matching_engine_client.h"

#include <chrono>
#include <iostream>
#include <memory>
#include <random>
#include <string>
#include <thread>
#include <fstream>
#include <condition_variable>

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

MatchingEngineClient::MatchingEngineClient(const std::string& uri)
    : stub_(MatchingEngine::NewStub(grpc::CreateChannel(uri, grpc::InsecureChannelCredentials()))) {}

unsigned int MatchingEngineClient::addOrder(const std::string& symbol, double price, unsigned int size, bool buy, bool limit) {
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

bool MatchingEngineClient::cancelOrder(const std::string& symbol, unsigned int orderID) {
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

void MatchingEngineClient::watchSymbol(const std::string& symbol, int watchDurationSeconds, bool log) {
    std::thread::id this_id = std::this_thread::get_id();
    grpc::ClientContext context;
    matchingengine::Symbol symbolRequest;
    symbolRequest.set_symbol(symbol);

    std::unique_ptr<grpc::ClientReader<matchingengine::SymbolData>> reader(
        stub_->WatchSymbol(&context, symbolRequest)
    );

    matchingengine::SymbolData data;
    auto startTime = std::chrono::steady_clock::now();
    std::ofstream outFile;

    if (log) {
        outFile.open(symbol + "_market_data.csv");
        outFile << "bid,bidSize,ask,askSize\n";  // CSV header
    }

    while (reader->Read(&data)) {
        // Process the data received from the server
        std::cout << "Thread ID " << this_id << ": " << "Bid: " << data.lastbid() << " Size: " << data.lastbidsize()
                  << ", Ask: " << data.lastask() << " Size: " << data.lastasksize() << std::endl;

        // Log the data to a CSV file if log is true
        if (log) {
            outFile << data.lastbid() << "," << data.lastbidsize() << ","
                    << data.lastask() << "," << data.lastasksize() << "\n";
        }

        // Check if the watch duration has elapsed
        auto currentTime = std::chrono::steady_clock::now();
        if (std::chrono::duration_cast<std::chrono::seconds>(currentTime - startTime).count() >= watchDurationSeconds) {
            // Signal the server to stop sending data
            context.TryCancel();
            break;
        }
    }

    grpc::Status status = reader->Finish();
    if (!status.ok()) {
        std::cerr << "WatchSymbol stream ended with error: " << status.error_message() << std::endl;
    } else {
        std::cout << "Finished watching symbol " << symbol << " for " << watchDurationSeconds << " seconds." << std::endl;
    }

    // Close the file if logging
    if (log) {
        outFile.close();
        std::cout << "Data saved to " << symbol + "_market_data.csv" << std::endl;
    }
}

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