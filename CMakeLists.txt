cmake_minimum_required(VERSION 3.15)
project(matching_engine)

find_program(PROTOC_PATH protoc REQUIRED)
find_program(GRPC_CPP_PLUGIN_PATH grpc_cpp_plugin REQUIRED)

set(CMAKE_CXX_STANDARD 17) # Use C++17
set(CMAKE_BUILD_TYPE Debug)
set(CMAKE_CXX_FLAGS "${CMAKE_CXX_FLAGS} -g")
# set(CMAKE_LINKER_FLAGS "${CMAKE_LINKER_FLAGS} -fsanitize=address")

find_package(Threads REQUIRED)
find_package(Protobuf REQUIRED)
find_package(gRPC CONFIG REQUIRED)

include_directories(${CMAKE_CURRENT_BINARY_DIR})

get_filename_component(me_proto "./matching_engine.proto" ABSOLUTE)
get_filename_component(me_proto_path "${me_proto}" PATH)

set(me_proto_srcs "${CMAKE_CURRENT_BINARY_DIR}/matching_engine.pb.cc")
set(me_proto_hdrs "${CMAKE_CURRENT_BINARY_DIR}/matching_engine.pb.h")
set(me_grpc_srcs "${CMAKE_CURRENT_BINARY_DIR}/matching_engine.grpc.pb.cc")
set(me_grpc_hdrs "${CMAKE_CURRENT_BINARY_DIR}/matching_engine.grpc.pb.h")

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

# Shared source files between client and server
set(SHARED_SOURCES
    src/core/order.cpp
    src/core/limit_price.cpp
    src/core/order_book.cpp
    src/core/matching_algorithm.cpp
    ${me_proto_srcs}
    ${me_grpc_srcs}
)

# Server specific source
set(SERVER_SOURCES
    src/grpc/matching_engine_server.cpp
)

# Client library source
set(CLIENT_LIBRARY_SOURCES
    src/grpc/matching_engine_client.cpp
)

# Create a library for the client interface
add_library(matching_engine_client_lib STATIC ${CLIENT_LIBRARY_SOURCES} ${SHARED_SOURCES})
target_link_libraries(matching_engine_client_lib
    gRPC::grpc++_reflection
    protobuf::libprotobuf
    Threads::Threads
)

# Add executable for server
add_executable(matching_engine_server ${SERVER_SOURCES} ${SHARED_SOURCES})
target_link_libraries(matching_engine_server
    gRPC::grpc++_reflection
    protobuf::libprotobuf
    Threads::Threads
)

# Client executables
# You can define multiple client executables that link against matching_engine_client_lib

# buyersellerclient
# add_executable(buyersellerclient src/clients/buyersellerclient.cpp)
# target_link_libraries(buyersellerclient matching_engine_client_lib)

# simpleclient
# add_executable(simpleclient src/clients/simpleclient.cpp)
# target_link_libraries(simpleclient matching_engine_client_lib)

# benchmark_test_1
# add_executable(benchmark_test_1 src/clients/benchmark_test_1.cpp)
# target_link_libraries(benchmark_test_1 matching_engine_client_lib)

# latency_test_1
# add_executable(latency_test_1 src/clients/latency_test_1.cpp)
# target_link_libraries(latency_test_1 matching_engine_client_lib)

# addlimit_ordering_test
# add_executable(addlimit_ordering_test src/clients/addlimit_ordering_test.cpp)
# target_link_libraries(addlimit_ordering_test matching_engine_client_lib)

# throughput_test_1
# add_executable(throughput_test_1 src/clients/throughput_test_1.cpp)
# target_link_libraries(throughput_test_1 matching_engine_client_lib)

# mean_reversion_client
add_executable(mean_reversion_client src/clients/mean_reversion_client.cpp)
target_link_libraries(mean_reversion_client matching_engine_client_lib)

# level2fetcher
add_executable(level2fetcher src/clients/level2fetcher.cpp)
target_link_libraries(level2fetcher matching_engine_client_lib)