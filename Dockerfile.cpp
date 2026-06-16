# syntax=docker/dockerfile:1

# ── Stage 1: build ──────────────────────────────────────────────────────────
FROM debian:bookworm-slim AS builder

RUN apt-get update && apt-get install -y --no-install-recommends \
    build-essential \
    cmake \
    libgrpc++-dev \
    protobuf-compiler-grpc \
    libprotobuf-dev \
    && rm -rf /var/lib/apt/lists/*

WORKDIR /app

# Copy build manifests before source so apt-layer stays cached on source changes
COPY CMakeLists.txt matching_engine.proto ./
COPY src/ src/

RUN cmake -B build && cmake --build build -j$(nproc)

# ── Stage 2: runtime ────────────────────────────────────────────────────────
FROM debian:bookworm-slim AS runtime

# libgrpc++1.51 pulls in libgrpc29, libabsl20220623, libssl3, libc-ares2, etc.
RUN apt-get update && apt-get install -y --no-install-recommends \
    libgrpc++1.51 \
    libprotobuf32 \
    && rm -rf /var/lib/apt/lists/*

WORKDIR /app
COPY --from=builder /app/build/matching_engine_server .
COPY --from=builder /app/build/mean_reversion_client .

EXPOSE 50051
CMD ["./matching_engine_server"]
