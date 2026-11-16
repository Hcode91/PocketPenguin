#!/bin/sh
# Builds the guest test programs with an ARM cross compiler and runs them on gp2xcli.
# Needs: gcc-arm-linux-gnueabi. Usage: tests/run_tests.sh ./gp2xcli
set -e
CLI=$(realpath "$1")
cd "$(dirname "$0")/guest"
CC="arm-linux-gnueabi-gcc -march=armv5te -O2 -static"
$CC -marm -pthread -o hello_static hello.c -lm
$CC -mthumb -pthread -o hello_thumb hello.c -lm
$CC -marm -o hwtest hwtest.c
$CC -marm -o oabi oabi.c
fail=0
check() { # name expected-substring output
    if echo "$3" | grep -q "$2"; then echo "PASS $1"; else echo "FAIL $1 (missing: $2)"; echo "$3"; fail=1; fi
}
OUT=$($CLI --quiet "$PWD/hello_static" || true)
for s in "malloc ok" "file: line 42" "slept ok" "threads counter=4000" "float 1.190" "dir entries ok" "done"; do check "hello/$s" "$s" "$OUT"; done
OUT=$($CLI --quiet "$PWD/hello_thumb" || true); check "thumb" "threads counter=4000" "$OUT"
OUT=$($CLI --quiet --model f100 "$PWD/oabi" || true)
for s in "oabi write ok" "oabi open/read ok" "oabi stat64 size=10" "oabi brk ok"; do check "$s" "$s" "$OUT"; done
for m in f100 wiz; do
    OUT=$($CLI --quiet --model $m -f 300 -k 30:0x1000 -k 32:0 -k 50:0x41 -k 52:0 "$PWD/hwtest" || true)
    check "hw/$m keys A" "keys 001000" "$OUT"
    check "hw/$m keys diagonal" "keys 0000c1" "$OUT"
    check "hw/$m finished" "done" "$OUT"
done
exit $fail
