#!/bin/bash
cat results/cc-time/$1/*.txt | grep "Register Allocator" | awk '{$1=$1};1' | cut -f1 -d' '
