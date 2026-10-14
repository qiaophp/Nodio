#!/usr/bin/env bash
# Run each aardio example headlessly and classify the outcome.
#
#   OK   = ran to completion (no error text)
#   ERR  = runtime/syntax error reported by the engine
#   HANG = did not exit within the timeout (GUI message loop, network wait)
#
# Paths are read NUL-separated from stdin so names containing spaces survive.
#
#   usage: find <dirs> -name '*.aardio' -print0 | tests/sweep_examples.sh <nodio.exe> [examplesRoot] [timeoutSecs]
#
# examplesRoot defaults to the aardio installation next to this repo
# (D:\aardio\examples); the library path is passed through NODIO_LIBDIR, so
# point that at an aardio installation's lib\ before running.
set -u
EXE="${1:?usage: ... | sweep_examples.sh <nodio.exe> [examplesRoot] [timeout]}"
ROOT="${2:-D:\\aardio\\examples}"
TMO="${3:-6}"
LIBDIR="${NODIO_LIBDIR:-D:\\aardio\\lib}"

OK=0; ERR=0; HANG=0; N=0
while IFS= read -r -d '' rel; do
  win="${rel//\//\\}"
  out=$(NODIO_RUN="$ROOT\\$win" NODIO_LIBDIR="$LIBDIR" NODIO_CONSOLE=1 \
        NODIO_PAUSE=0 timeout "$TMO" "$EXE" 2>&1)
  rc=$?
  N=$((N+1))
  if [ $rc -eq 124 ]; then
    HANG=$((HANG+1)); echo "HANG  $rel"
  elif printf '%s' "$out" | grep -q '运行错误\|语法错误\|无法打开\|编译失败\|无法加载'; then
    ERR=$((ERR+1))
    echo "ERR   $rel :: $(printf '%s' "$out" | grep -m1 '运行错误\|语法错误\|无法打开\|编译失败\|无法加载' | cut -c1-100)"
  else
    OK=$((OK+1)); echo "OK    $rel"
  fi
done
echo "== ran=$N ok=$OK err=$ERR hang=$HANG =="
