#!/bin/bash
set -euo pipefail

echo "[BUILD] Building gwatch and sample target..."
g++ -std=c++14 -O2 -Wall -g -o gwatch gwatch.cpp
g++ -std=c++14 -O2 -Wall -g -o sample_target sample_target.cpp

# Sanity: symbol exists
if ! nm sample_target | grep -q " watched$"; then
  echo "[ERROR] Symbol 'watched' not found in sample_target!" >&2
  exit 1
fi

echo "[TEST] Measuring baseline performance..."
T_DIRECT=$( (time -p ./sample_target > /dev/null) 2>&1 | awk '/real/ {print $2}' )

echo "[TEST] Measuring gwatch performance (output -> /dev/null to avoid I/O noise)..."
# Redirect gwatch stdout+stderr to /dev/null for timing measurement
T_GWATCH=$( (time -p ./gwatch --var watched --exec ./sample_target > /dev/null 2>/dev/null) 2>&1 | awk '/real/ {print $2}' )

echo "[TEST] Running gwatch for functional check (capture small sample)..."
# Produce functional output into out.txt (we'll restrict how many lines we capture)
# Limit runtime by running for a short time (optional): run normally and then kill after a short sleep OR rely on sample_target finishing quickly.
./gwatch --var watched --exec ./sample_target > out.txt 2>gwatch.err || true

# Functional check: must contain at least one 'write' and one 'read'
if grep -q "write" out.txt && grep -q "read" out.txt; then
  echo "[PASS] Functional check: found read/write events."
else
  echo "[FAIL] Functional check: missing expected events!" >&2
  echo "---- out.txt ----"
  cat out.txt
  echo "---- gwatch.err ----"
  cat gwatch.err
  exit 2
fi

echo "[INFO] Direct: $T_DIRECT s, gwatch (timed without I/O): $T_GWATCH s"
RATIO=$(awk -v d="$T_DIRECT" -v g="$T_GWATCH" 'BEGIN{ if (d==0) { print "inf"; } else printf "%.2f", g/d }')

echo "[RESULT] slowdown ratio: $RATIO"

# Check numeric ratio conservatively
if [ "$RATIO" = "inf" ]; then
    echo "[WARN] baseline time was 0, cannot compute ratio reliably; please increase sample_target workload." >&2
else
    if (( $(echo "$RATIO <= 2.0" | bc -l) )); then
      echo "[PASS] Performance check: OK (≤2× slowdown)."
    else
      echo "[FAIL] Performance check: gwatch too slow (>2×)." >&2
      exit 3
    fi
fi

echo "[SUCCESS] All tests passed"
