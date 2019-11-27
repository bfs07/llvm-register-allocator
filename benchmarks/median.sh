#!/bin/bash
exec awk '{x[NR]=$0} END{middle=int(NR/2); print x[middle]}'
