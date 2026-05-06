#!/usr/bin/env bash
set -euo pipefail

KREP_BIN="${KREP_BIN:-./krep}"
TMP_DIR="$(mktemp -d)"
trap 'rm -rf "${TMP_DIR}"' EXIT

DATASET="${TMP_DIR}/simd-dispatch.txt"
PATTERN="${PATTERN:-needle16}"
RUNS="${RUNS:-3}"

python3 - "${DATASET}" "${PATTERN}" <<'PY'
import sys

path, pattern = sys.argv[1], sys.argv[2]
chunk = 2 * 1024 * 1024
with open(path, "wb") as f:
    for i in range(24):
        fill = b"a" if i % 2 == 0 else b"b"
        f.write(fill * (chunk - len(pattern)))
        f.write(pattern.encode())
    f.write(b"\n")
PY

if [[ ! -x "${KREP_BIN}" ]]; then
  echo "krep binary not found or not executable at: ${KREP_BIN}" >&2
  exit 1
fi

single_count="$("${KREP_BIN}" -t 1 -o -F -- "${PATTERN}" "${DATASET}" | wc -l | tr -d ' ')"
multi_count="$("${KREP_BIN}" -t 4 -o -F -- "${PATTERN}" "${DATASET}" | wc -l | tr -d ' ')"
nosimd_count="$("${KREP_BIN}" --no-simd -t 4 -o -F -- "${PATTERN}" "${DATASET}" | wc -l | tr -d ' ')"

if [[ "${single_count}" != "${multi_count}" || "${single_count}" != "${nosimd_count}" ]]; then
  echo "Mismatch for -o overlap/runtime dispatch counts: single=${single_count}, multi=${multi_count}, no-simd=${nosimd_count}" >&2
  exit 1
fi

measure_avg() {
  python3 - "${RUNS}" "$@" <<'PY'
import subprocess
import sys
import time

runs = int(sys.argv[1])
cmd = sys.argv[2:]
total = 0.0
for _ in range(runs):
    start = time.perf_counter()
    subprocess.run(cmd, stdout=subprocess.DEVNULL, check=True)
    total += time.perf_counter() - start
print(f"{total / runs:.6f}")
PY
}

simd_avg="$(measure_avg "${KREP_BIN}" -c -F -- "${PATTERN}" "${DATASET}")"
nosimd_avg="$(measure_avg "${KREP_BIN}" --no-simd -c -F -- "${PATTERN}" "${DATASET}")"
ratio="$(awk -v s="${simd_avg}" -v n="${nosimd_avg}" 'BEGIN { if (s > 0) printf "%.2f", n / s; else print "inf" }')"
verdict="$(awk -v s="${simd_avg}" -v n="${nosimd_avg}" 'BEGIN { print (s <= n) ? "better-or-equal" : "worse" }')"

printf "SIMD dispatch smoke test\n"
printf "Pattern: %s\n" "${PATTERN}"
printf "Validated -o count: %s\n" "${single_count}"
printf "SIMD avg real: %s\n" "${simd_avg}"
printf "No-SIMD avg real: %s\n" "${nosimd_avg}"
printf "No-SIMD/SIMD ratio: %sx\n" "${ratio}"
printf "Result: %s\n" "${verdict}"
