#!/bin/bash
cat results/bin-size/$1/*.txt | tail -n1 | cut -f5 -d' '
