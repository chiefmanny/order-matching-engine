#!/bin/sh
# Reproduces the benchmarks. Run from the repository root with Docker running.
set -eu

COARSE_REF=c8c3aac   # last hand-written commit, coarse locking
FINE_REF=ae5e9de     # last hand-written commit, fine-grained locking
CURRENT_REF=${CURRENT_REF:-HEAD}

here=docs/benchmarks
stage=$(mktemp -d)
trap 'rm -rf "$stage"' EXIT

for pair in "coarse $COARSE_REF" "fine $FINE_REF" "current $CURRENT_REF"; do
    set -- $pair
    mkdir -p "$stage/sources/$1"
    git archive "$2" | tar -x -C "$stage/sources/$1"
done
cp "$here/Dockerfile" "$here/build_version.sh" "$here/run_benchmarks.sh" "$stage/"
chmod +x "$stage/build_version.sh" "$stage/run_benchmarks.sh"

docker build -t ome-bench "$stage"
mkdir -p "$here/results"
docker run --rm -v "$PWD/$here/results:/results" \
    -e VERSIONS -e THROUGHPUT_RUNS -e LATENCY_RUNS -e MAX_SIZE ome-bench
