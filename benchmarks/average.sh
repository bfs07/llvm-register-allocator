#!/bin/bash
exec awk '{ total += $1; count++ } END { print total/count }'