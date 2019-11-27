#!/bin/bash
export TCCGEN_SRC='tccgen.c'
export TCCGEN_DEFINES='-DCONFIG_TRIPLET="\"x86_64-linux-gnu\"" -DTCC_TARGET_X86_64       -DONE_SOURCE=0'
export TCCGEN_CFLAGS='-O0 -Wdeclaration-after-statement -fno-strict-aliasing -Wno-pointer-sign -Wno-sign-compare -Wno-unused-result -Wno-format-truncation -fPIC -I. '
