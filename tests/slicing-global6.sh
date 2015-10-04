#!/bin/bash

# make bash exit on any failure
set -e

TESTS_DIR=`dirname $0`
source "$TESTS_DIR/test-runner.sh"

set_environment

CODE="$TESTS_DIR/sources/global6.c"
NAME=${CODE%.*}
BCFILE="$NAME.bc"
SLICEDFILE="$NAME.sliced"

clang -emit-llvm -c "$CODE" -o "$BCFILE"
llvm-slicer -c __assert_fail "$BCFILE"

# timeout
pid=$$
(sleep 5; kill -9 $pid; pkill -9 lli) &
watchpid=$!

# run the sliced code and check if it
# has everything to pass the assert in it
lli "$SLICEDFILE" && "assert not reached, failure"
kill $watchpid
exit 0
