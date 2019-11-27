#!/bin/bash
set -e

function build_tinycc {
	echo "Building tinycc with register allocator $1 (flags: $2)..."
	cd tinycc
	export CFLAGS=-O0
	PREFIX=$(readlink -f "../build/tinycc-$1")
	LLC_FLAGS="-relocation-model=pic -time-passes $2"
	mkdir "$PREFIX"
	make clean
	./configure --prefix="$PREFIX"
	make -f Makefile.custom  LLC_FLAGS="$LLC_FLAGS" &>"$PREFIX/build-log"
	make -f Makefile.custom  LLC_FLAGS="$LLC_FLAGS" test
	make -f Makefile.custom  LLC_FLAGS="$LLC_FLAGS" install
	cd ..
}

rm -rf build
mkdir build
build_tinycc "llvm-fast" "-regalloc=fast"
build_tinycc "llvm-pbqp" "-regalloc=pbqp"
build_tinycc "llvm-greedy" "-regalloc=greedy"
build_tinycc "llvm-basic" "-regalloc=basic"
build_tinycc "oidara" "-load ../../src/oidara-algorithm/libRegAllocColor.so -regalloc=colorBased"
build_tinycc "ours" "-load ../../src/our-algorithm/libRegAllocColor.so -regalloc=colorBased"
