#!/bin/bash
set -e

source tccgen-flags.sh
TINYCC_SRC="$TCCGEN_SRC"
DEFINES="$TCCGEN_DEFINES"
CFLAGS="-O2 $TCCGEN_CFLAGS -O2"
RESULTS_BASE_DIR="results/tcc-time"

function run_bench_iteration() {
	echo "Running for $1 (flags: $2)..."
	TCC_EXEC="./build/tinycc-$1/bin/tcc"
	rm -f /tmp/a.o
	set -x
	time "$TCC_EXEC" -o /tmp/a.o -c "tinycc/$TINYCC_SRC" $DEFINES $CFLAGS
	set +x
}

function run_bench() {
	RESULTS_DIR="$RESULTS_BASE_DIR/$1"
	mkdir "$RESULTS_DIR"
	echo "Running benchmark for $1 ($RESULTS_DIR)..."
	for i in $(seq 1 10); do
		echo "Running $i-th warm-up..."
		run_bench_iteration "$1" "$2" &>/dev/null
	done
	for i in $(seq 1 20); do
		echo "Running $i-th iteration..."
		run_bench_iteration "$1" "$2" &>"$RESULTS_DIR/$i.txt"
	done
}

rm -rf "$RESULTS_BASE_DIR"
mkdir -p "$RESULTS_BASE_DIR"
run_bench "llvm-fast" "-regalloc=fast"
run_bench "llvm-pbqp" "-regalloc=pbqp"
run_bench "llvm-greedy" "-regalloc=greedy"
run_bench "llvm-basic" "-regalloc=basic"
run_bench "oidara" "-load ../src/oidara-algorithm/libRegAllocColor.so -regalloc=colorBased"
run_bench "ours" "-load ../src/our-algorithm/libRegAllocColor.so -regalloc=colorBased"
