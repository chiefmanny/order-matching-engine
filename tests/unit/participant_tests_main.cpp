//
//  participant_tests_main.cpp
//
//  Fast unit tests for the market-participant client infrastructure.
//  These do not require a live matching engine or gRPC, and run in milliseconds.
//  (The legacy order-book stress tests live in the separate order_book_tests
//  target, which spins up many threads and produces heavy output.)
//

#include <iostream>

void run_event_log_tests();
void run_liquidity_provider_tests();
void run_directional_trader_tests();
void run_market_maker_tests();
void run_noise_trader_tests();
void run_order_tracker_tests();
void run_sweep_tests();
void run_tick_math_tests();
void run_participant_config_tests();
void run_position_tracker_tests();
void run_random_source_tests();

int main() {
    std::cout << "=== Participant infrastructure unit tests ===" << std::endl;
    run_event_log_tests();
    run_liquidity_provider_tests();
    run_directional_trader_tests();
    run_market_maker_tests();
    run_noise_trader_tests();
    run_sweep_tests();
    run_tick_math_tests();
    run_order_tracker_tests();
    run_participant_config_tests();
    run_position_tracker_tests();
    run_random_source_tests();
    std::cout << "=== All participant tests passed ===" << std::endl;
    return 0;
}
