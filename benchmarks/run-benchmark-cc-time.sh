#!/bin/bash
set -e

source tccgen-flags.sh
TINYCC_SRC="$TCCGEN_SRC"
DEFINES="$TCCGEN_DEFINES"
CFLAGS="$TCCGEN_CFLAGS"
RESULTS_BASE_DIR="results/cc-time"

function run_bench_iteration() {
	echo "Running for $1 (flags: $2)..."
	BC_FILE="/tmp/$TINYCC_SRC.bc"
	LLC_FLAGS="-relocation-model=pic -time-passes $2"
	set -x
	clang-4.0 -emit-llvm -o "$BC_FILE" -c "tinycc/$TINYCC_SRC" $DEFINES $CFLAGS
	llc-4.0 $LLC_FLAGS "$BC_FILE" -filetype=obj -o /dev/null
	set +x
	rm "$BC_FILE"
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
