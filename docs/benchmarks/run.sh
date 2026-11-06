#!/bin/sh
# Reproduces the benchmarks. Run from the repository root with Docker running.
#   docs/benchmarks/run.sh                  latency and throughput sweep
#   docs/benchmarks/run.sh asan_repro.sh    AddressSanitizer reproduction
# On a Mac, prefix with `caffeinate -i`: if the machine sleeps, the timings are invalid.
set -eu

COARSE_REF=c8c3aac   # last hand-written commit, coarse locking
FINE_REF=ae5e9de     # last hand-written commit, fine-grained locking
CURRENT_REF=${CURRENT_REF:-HEAD}

here=docs/benchmarks
stage=$(mktemp -d)
trap 'rm -rf "$stage"' EXIT

stage_version() {
    mkdir -p "$stage/sources/$1"
    git archive "$2" | tar -x -C "$stage/sources/$1"
}
stage_version coarse "$COARSE_REF"
stage_version fine "$FINE_REF"
stage_version current "$CURRENT_REF"
cp "$here/Dockerfile" "$here"/*.sh "$stage/"
chmod +x "$stage"/*.sh

docker build -t ome-bench "$stage"
mkdir -p "$here/results"
docker run --rm -v "$PWD/$here/results:/results" \
    -e VERSIONS -e THROUGHPUT_RUNS -e LATENCY_RUNS -e MAX_SIZE ome-bench "$@"
