# Builds the coarse-locking, fine-grained, and current engines with one
# toolchain and one set of flags, so differences come from the code.
FROM debian:bookworm-slim

RUN apt-get update && apt-get install -y --no-install-recommends \
    build-essential cmake libgrpc++-dev protobuf-compiler-grpc \
    libprotobuf-dev nlohmann-json3-dev procps \
    && rm -rf /var/lib/apt/lists/*

COPY sources/ /sources/
COPY build_version.sh run_benchmarks.sh /usr/local/bin/
RUN for v in coarse fine current; do build_version.sh "$v"; done

CMD ["run_benchmarks.sh"]
