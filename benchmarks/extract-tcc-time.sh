#!/bin/bash
cat results/tcc-time/$1/*.txt | grep user | cut -f2 | cut -c 3-7
