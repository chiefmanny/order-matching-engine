#!/bin/sh
python -m grpc_tools.protoc -I=. --python_out=. --grpc_python_out=. matching_engine.proto
mkdir -p build && cd build
cmake -DCMAKE_PREFIX_PATH=$HOME/.local ..
