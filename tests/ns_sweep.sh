#!/usr/bin/env bash
# Namespace import coverage: import every top-level official-lib
# namespace headlessly and record pass/fail. This is the reproducible
# script behind the "Library import coverage" table in docs/roadmap.md.
#
# Usage: tests/ns_sweep.sh <path-to-nodio.exe> [libdir]
#   tests/ns_sweep.sh build-x64/Release/nodio.exe
#   tests/ns_sweep.sh build-x86/Release/nodio.exe D:\\aardio\\lib
set -u
EXE="${1:?usage: ns_sweep.sh <nodio.exe> [libdir]}"
LIBDIR="${2:-D:\\aardio\\lib}"
PROBE="${TMP:-/tmp}/nsprobe.$$.aardio"
trap 'rm -f "$PROBE"' EXIT

OK=0; FAIL=0
for d in /d/aardio/lib/*/; do
  name=$(basename "$d")
  printf 'import %s;\nio.print("ns-ok");\n' "$name" > "$PROBE"
  # </dev/null so an import that reads stdin cannot eat the loop's input;
  # timeout guards against imports that wait on the network.
  out=$(NODIO_RUN="$(cygpath -w "$PROBE")" NODIO_LIBDIR="$LIBDIR" \
        NODIO_CONSOLE=1 NODIO_PAUSE=0 timeout 20 "$EXE" </dev/null 2>&1)
  rc=$?
  if [ $rc -eq 0 ] && ! printf '%s' "$out" | grep -q '运行错误\|语法错误\|无法打开\|无法加载\|找不到'; then
    OK=$((OK+1)); printf 'ok    %s\n' "$name"
  else
    FAIL=$((FAIL+1))
    msg=$(printf '%s' "$out" | grep -m1 '错误\|找不到\|无法' | head -c 120)
    printf 'FAIL  %s :: %s\n' "$name" "${msg:-exit $rc}"
  fi
done
echo "== ok=$OK fail=$FAIL =="
