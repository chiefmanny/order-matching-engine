cmake_minimum_required(VERSION 3.15)
project(matching_engine)

set(PROTOC_PATH "/Users/mananmanchanda/.local/bin/protoc")
set(GRPC_CPP_PLUGIN_PATH "/Users/mananmanchanda/.local/bin/grpc_cpp_plugin")

set(CMAKE_CXX_STANDARD 17) # Use C++17

find_package(Threads REQUIRED)
find_package(Protobuf CONFIG REQUIRED)
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
    order.cpp
    limit_price.cpp
    order_book.cpp
    matching_algorithm.cpp
    ${me_proto_srcs}
    ${me_grpc_srcs}
)

# Server specific source
set(SERVER_SOURCES
    matching_engine_server.cpp
)

# Client library source
set(CLIENT_LIBRARY_SOURCES
    matching_engine_client.cpp
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
add_executable(buyersellerclient buyersellerclient.cpp)
target_link_libraries(buyersellerclient matching_engine_client_lib)