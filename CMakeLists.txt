cmake_minimum_required(VERSION 3.15)
project(matching_engine)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_BUILD_TYPE Debug)
set(CMAKE_CXX_FLAGS "${CMAKE_CXX_FLAGS} -g")
# set(CMAKE_LINKER_FLAGS "${CMAKE_LINKER_FLAGS} -fsanitize=address")

find_package(Threads REQUIRED)

# nlohmann/json — header-only JSON library used by participant_base_lib.
# Prefer a system package (Debian: nlohmann-json3-dev) so Docker/offline builds
# need no network; fall back to fetching from GitHub only when it is absent.
find_package(nlohmann_json 3.9 QUIET)
if(NOT nlohmann_json_FOUND)
    message(STATUS "nlohmann_json not found as a system package — fetching from GitHub")
    include(FetchContent)
    FetchContent_Declare(
        nlohmann_json
        GIT_REPOSITORY https://github.com/nlohmann/json.git
        GIT_TAG        v3.11.3
        GIT_SHALLOW    TRUE
    )
    FetchContent_MakeAvailable(nlohmann_json)
endif()

# ===========================================================================
# core_lib — order book + matching algorithm, no gRPC or proto dependency.
# Lets unit tests build without a local gRPC installation.
# ===========================================================================
set(CORE_SOURCES
    src/core/order.cpp
    src/core/limit_price.cpp
    src/core/order_book.cpp
    src/core/matching_algorithm.cpp
)

add_library(core_lib STATIC ${CORE_SOURCES})
target_include_directories(core_lib PUBLIC ${CMAKE_SOURCE_DIR}/src/core)
target_link_libraries(core_lib PUBLIC Threads::Threads)

# ===========================================================================
# participant_base_lib — order tracking + config.
# No gRPC dependency; used by both unit_tests and participant_common_lib.
# ===========================================================================
set(PARTICIPANT_BASE_SOURCES
    src/clients/common/event_log.cpp
    src/clients/common/strategies/echo_strategy.cpp
    src/clients/common/strategies/liquidity_provider.cpp
    src/clients/common/strategies/directional_trader.cpp
    src/clients/common/strategies/market_maker.cpp
    src/clients/common/strategies/noise_trader.cpp
    src/clients/common/strategies/parent_executor.cpp
    src/clients/common/order_tracker.cpp
    src/clients/common/participant_config.cpp
    src/clients/common/position_tracker.cpp
)

add_library(participant_base_lib STATIC ${PARTICIPANT_BASE_SOURCES})
target_include_directories(participant_base_lib
    PUBLIC ${CMAKE_SOURCE_DIR}/src/clients/common
)
target_link_libraries(participant_base_lib
    PUBLIC nlohmann_json::nlohmann_json
)

# ===========================================================================
# Tests — buildable without gRPC.
# Guarded on the tests/ directory existing, since some build contexts
# (e.g. Dockerfile.cpp) copy only src/ and would otherwise fail to find them.
# ===========================================================================
if(EXISTS ${CMAKE_SOURCE_DIR}/tests/unit/participant_tests_main.cpp)
    enable_testing()

    # participant_tests — fast unit tests for the client infrastructure.
    # Run these in CI; they complete in milliseconds with no threads or engine.
    add_executable(participant_tests
        tests/unit/participant_tests_main.cpp
        tests/unit/test_event_log.cpp
        tests/unit/test_liquidity_provider.cpp
        tests/unit/test_directional_trader.cpp
        tests/unit/test_market_maker.cpp
        tests/unit/test_noise_trader.cpp
        tests/unit/test_parent_executor.cpp
        tests/unit/test_tick_math.cpp
        tests/unit/test_order_tracker.cpp
        tests/unit/test_participant_config.cpp
        tests/unit/test_position_tracker.cpp
        tests/unit/test_random_source.cpp
    )
    target_include_directories(participant_tests
        PRIVATE ${CMAKE_SOURCE_DIR}/tests/unit
    )
    target_link_libraries(participant_tests participant_base_lib)
    add_test(NAME participant_tests COMMAND participant_tests)

    # book_invariant_tests — fast checks on order-book depth bookkeeping and the
    # trade tape (maker pricing, aggressor side, fill fan-out).  No gRPC needed.
    add_executable(book_invariant_tests
        tests/unit/test_book_invariants.cpp
    )
    target_include_directories(book_invariant_tests
        PRIVATE ${CMAKE_SOURCE_DIR}/src/core
    )
    target_link_libraries(book_invariant_tests core_lib)
    add_test(NAME book_invariant_tests COMMAND book_invariant_tests)

    # order_book_tests — legacy multithreaded stress tests for the core engine.
    # Slow and verbose; kept building but not part of the default CI fast path.
    add_executable(order_book_tests
        tests/unit/main.cpp
        tests/unit/order_book_tests.cpp
    )
    target_include_directories(order_book_tests
        PRIVATE
            ${CMAKE_SOURCE_DIR}/src/core
            ${CMAKE_SOURCE_DIR}/tests/unit
    )
    target_link_libraries(order_book_tests core_lib)
    add_test(NAME order_book_tests COMMAND order_book_tests)
endif()

# ===========================================================================
# gRPC-dependent targets — only built when gRPC is available.
# In Docker (Dockerfile.cpp) all tools are present.  Local dev machines
# without a gRPC installation will skip this block and still build unit_tests.
# ===========================================================================
find_program(PROTOC_PATH protoc)
find_program(GRPC_CPP_PLUGIN_PATH grpc_cpp_plugin)
find_package(Protobuf QUIET)
find_package(gRPC CONFIG QUIET)

if(gRPC_FOUND AND Protobuf_FOUND AND PROTOC_PATH AND GRPC_CPP_PLUGIN_PATH)
    message(STATUS "gRPC found — building server and client executables")

    include_directories(${CMAKE_CURRENT_BINARY_DIR})

    get_filename_component(me_proto "./matching_engine.proto" ABSOLUTE)
    get_filename_component(me_proto_path "${me_proto}" PATH)

    set(me_proto_srcs "${CMAKE_CURRENT_BINARY_DIR}/matching_engine.pb.cc")
    set(me_proto_hdrs "${CMAKE_CURRENT_BINARY_DIR}/matching_engine.pb.h")
    set(me_grpc_srcs  "${CMAKE_CURRENT_BINARY_DIR}/matching_engine.grpc.pb.cc")
    set(me_grpc_hdrs  "${CMAKE_CURRENT_BINARY_DIR}/matching_engine.grpc.pb.h")

    add_custom_command(
        OUTPUT "${me_proto_srcs}" "${me_proto_hdrs}" "${me_grpc_srcs}" "${me_grpc_hdrs}"
        COMMAND ${PROTOC_PATH}
        ARGS --grpc_out "${CMAKE_CURRENT_BINARY_DIR}"
             --cpp_out "${CMAKE_CURRENT_BINARY_DIR}"
             -I "${me_proto_path}"
             --plugin=protoc-gen-grpc="${GRPC_CPP_PLUGIN_PATH}"
             "${me_proto}"
        DEPENDS "${me_proto}"
    )

    set(SHARED_SOURCES
        src/core/order.cpp
        src/core/limit_price.cpp
        src/core/order_book.cpp
        src/core/matching_algorithm.cpp
        ${me_proto_srcs}
        ${me_grpc_srcs}
    )

    set(SERVER_SOURCES src/grpc/matching_engine_server.cpp)
    set(CLIENT_LIBRARY_SOURCES src/grpc/matching_engine_client.cpp)

    add_library(matching_engine_client_lib STATIC ${CLIENT_LIBRARY_SOURCES} ${SHARED_SOURCES})
    target_link_libraries(matching_engine_client_lib
        gRPC::grpc++_reflection
        protobuf::libprotobuf
        Threads::Threads
    )

    add_executable(matching_engine_server ${SERVER_SOURCES} ${SHARED_SOURCES})
    target_link_libraries(matching_engine_server
        gRPC::grpc++_reflection
        protobuf::libprotobuf
        Threads::Threads
    )

    # --- Disabled legacy client executables ---
    # add_executable(buyersellerclient src/clients/buyersellerclient.cpp)
    # target_link_libraries(buyersellerclient matching_engine_client_lib)
    # add_executable(simpleclient src/clients/simpleclient.cpp)
    # target_link_libraries(simpleclient matching_engine_client_lib)
    # add_executable(benchmark_test_1 src/clients/benchmark_test_1.cpp)
    # target_link_libraries(benchmark_test_1 matching_engine_client_lib)
    # add_executable(latency_test_1 src/clients/latency_test_1.cpp)
    # target_link_libraries(latency_test_1 matching_engine_client_lib)
    # add_executable(addlimit_ordering_test src/clients/addlimit_ordering_test.cpp)
    # target_link_libraries(addlimit_ordering_test matching_engine_client_lib)
    # add_executable(throughput_test_1 src/clients/throughput_test_1.cpp)
    # target_link_libraries(throughput_test_1 matching_engine_client_lib)

    add_executable(mean_reversion_client src/clients/mean_reversion_client.cpp)
    target_link_libraries(mean_reversion_client matching_engine_client_lib)

    add_executable(level2fetcher src/clients/level2fetcher.cpp)
    target_link_libraries(level2fetcher matching_engine_client_lib)

    # participant_common_lib — interface target combining base lib + gRPC client.
    # Participant executables link against this rather than listing both deps.
    add_library(participant_common_lib INTERFACE)
    target_link_libraries(participant_common_lib
        INTERFACE
            participant_base_lib
            matching_engine_client_lib
    )

    # =======================================================================
    # participant_runtime_lib — the gRPC-dependent half of the participant
    # framework: engine session, runtime lifecycle, and strategy implementations.
    #
    # Kept separate from participant_base_lib on purpose: base stays gRPC-free so
    # participant_tests builds and runs on machines with no gRPC installed, and so
    # strategies remain testable against a fake OrderGateway.
    # =======================================================================
    set(PARTICIPANT_RUNTIME_SOURCES
        src/clients/common/engine_session.cpp
        src/clients/common/participant_runtime.cpp
    )

    add_library(participant_runtime_lib STATIC
        ${PARTICIPANT_RUNTIME_SOURCES}
        ${me_proto_srcs}
        ${me_grpc_srcs}
    )
    target_include_directories(participant_runtime_lib
        PUBLIC
            ${CMAKE_SOURCE_DIR}/src/clients/common
            ${CMAKE_CURRENT_BINARY_DIR}
    )
    target_link_libraries(participant_runtime_lib
        PUBLIC
            participant_base_lib
            gRPC::grpc++_reflection
            protobuf::libprotobuf
            Threads::Threads
    )

    # participant_runner — the ONE participant executable. `strategy` in the config
    # selects the behavior; new participants never need a new target here.
    add_executable(participant_runner src/clients/participant_runner.cpp)
    target_link_libraries(participant_runner participant_runtime_lib)

    # market_recorder — the harness's independent observer process.
    add_executable(market_recorder src/clients/market_recorder.cpp)
    target_link_libraries(market_recorder participant_runtime_lib)

else()
    message(STATUS "gRPC not found — skipping server and client executables (unit_tests still builds)")
endif()
