#!/usr/bin/env bash
# Comprehensive grep vs krep benchmark across all major scenarios
set -euo pipefail

KREP="${KREP:-/Users/sorda/.local/bin/krep}"
GREP="${GREP:-grep}"
RG="${RG:-rg}"
RUNS="${RUNS:-5}"
TMPDIR_BENCH="$(mktemp -d /tmp/bench_krep_XXXXXX)"
trap 'rm -rf "$TMPDIR_BENCH"' EXIT

RED='\033[0;31m'; GREEN='\033[0;32m'; YELLOW='\033[1;33m'
CYAN='\033[0;36m'; BOLD='\033[1m'; RESET='\033[0m'

if ! command -v "$RG" &>/dev/null; then
    RG=""
fi

HR="────────────────────────────────────────────────────────────────────────────"

# ── helpers ──────────────────────────────────────────────────────────────────

measure() {
  # measure N runs of "$@", return median real-time in seconds
  local runs="$1"; shift
  local t sum=0 count=0
  local timefile
  timefile=$(mktemp /tmp/bench_time_XXXXXX)
  for ((i=0; i<runs; i++)); do
    { /usr/bin/time -p "$@" >/dev/null; } 2>"$timefile" || true
    t=$(awk '/^real/{print $2}' "$timefile")
    if [[ -n "$t" ]]; then
      echo "$t" >> "${timefile}.all"
      count=$((count+1))
    fi
  done
  rm -f "$timefile"
  if [[ $count -eq 0 ]]; then echo "0"; rm -f "${timefile}.all"; return; fi
  sort -n "${timefile}.all" | awk -v n="$count" 'NR==int(n/2)+1{print}'
  rm -f "${timefile}.all"
}

measure_ms() {
  # like measure but prints milliseconds
  local t
  t=$(measure "$@")
  awk -v t="$t" 'BEGIN{printf "%.1f", t*1000}'
}

rg_ms() {
  # measure rg; returns empty string if rg is not available
  if [[ -z "$RG" ]]; then echo ""; return; fi
  measure_ms "$@"
}

winner() {
  local k="$1" g="$2" r="${3:-}"
  awk -v k="$k" -v g="$g" -v r="$r" \
      -v GREEN="$GREEN" -v YELLOW="$YELLOW" -v CYAN="$CYAN" -v RESET="$RESET" '
  BEGIN {
    if (k+0==0 || g+0==0) { print "N/A"; exit }
    best=k; wname="krep"
    if (g+0>0 && g<best) { best=g; wname="grep" }
    if (r+0>0 && r<best) { best=r; wname="rg"   }
    if (wname=="krep") {
      printf GREEN "krep fastest"
      if (g+0>0) printf " (%.1fx grep)", g/k
      if (r+0>0) printf " (%.1fx rg)", r/k
    } else if (wname=="rg") {
      printf CYAN "rg fastest"
      if (k+0>0) printf " (%.1fx krep)", k/r
      if (g+0>0) printf " (%.1fx grep)", g/r
    } else {
      printf YELLOW "grep fastest"
      if (k+0>0) printf " (%.1fx krep)", k/g
      if (r+0>0) printf " (%.1fx rg)", r/g
    }
    printf RESET "\n"
  }'
}

print_row() {
  local scenario="$1" k_ms="$2" g_ms="$3" r_ms="${4:-}"
  local w
  w=$(winner "$k_ms" "$g_ms" "$r_ms")
  if [[ -n "$r_ms" ]]; then
    printf "  %-40s  krep=%6s ms  grep=%6s ms    rg=%6s ms  %b\n" \
      "$scenario" "$k_ms" "$g_ms" "$r_ms" "$w"
  else
    printf "  %-40s  krep=%6s ms  grep=%6s ms  %b\n" \
      "$scenario" "$k_ms" "$g_ms" "$w"
  fi
}

# ── generate test corpora ─────────────────────────────────────────────────────

echo -e "${BOLD}Generating test corpora...${RESET}"

# 50 MB plain text (english-like words)
BIG="$TMPDIR_BENCH/big.txt"
python3 -c "
import random, string, sys
words = ['the','quick','brown','fox','jumps','over','lazy','dog',
         'error','warning','info','debug','failed','success','timeout',
         'connection','request','response','server','client','data',
         'file','path','user','admin','password','token','key','value']
lines = []
for _ in range(1_200_000):
    n = random.randint(5,12)
    lines.append(' '.join(random.choices(words, k=n)))
sys.stdout.write('\n'.join(lines)+'\n')
" > "$BIG"
BIG_SIZE=$(du -sh "$BIG" | cut -f1)

# 5 MB log-style file (timestamps + levels + messages)
LOG="$TMPDIR_BENCH/app.log"
python3 -c "
import random
levels=['INFO','WARNING','ERROR','DEBUG','CRITICAL']
msgs=['Connection refused','Timeout expired','File not found',
      'Authentication failed','Request processed','Cache hit',
      'Disk full','Memory limit reached','Segmentation fault',
      'Stack overflow','Null pointer dereference','Access denied']
for i in range(100_000):
    lvl=random.choice(levels)
    msg=random.choice(msgs)
    print(f'2026-05-06 {i//3600:02d}:{(i%3600)//60:02d}:{i%60:02d} [{lvl}] {msg} (req_id={i})')
" > "$LOG"

# 2 MB file: many lines matching (high-density)
DENSE="$TMPDIR_BENCH/dense.txt"
python3 -c "
for i in range(200_000):
    print(f'match_{i%100} value={i} status=ok' if i%3==0 else f'no_hit line={i}')
" > "$DENSE"

# sparse match file (1 match per 10000 lines)
SPARSE="$TMPDIR_BENCH/sparse.txt"
python3 -c "
for i in range(500_000):
    print('NEEDLE_UNIQUE' if i%10000==0 else f'haystack line {i} with random text abcdefgh')
" > "$SPARSE"

# source code tree (use krep project itself, copy to tmp)
SRCDIR="$TMPDIR_BENCH/src"
cp -r /Users/sorda/Projects/krep "$SRCDIR"

echo -e "  big.txt:    ${BIG_SIZE}  (plain text, 1.2M lines)"
echo -e "  app.log:    $(du -sh "$LOG"|cut -f1)  (log file, 100K lines)"
echo -e "  dense.txt:  $(du -sh "$DENSE"|cut -f1)  (33% lines match)"
echo -e "  sparse.txt: $(du -sh "$SPARSE"|cut -f1)  (1 match per 10K lines)"
echo ""

# ── warm up OS page cache ─────────────────────────────────────────────────────
for f in "$BIG" "$LOG" "$DENSE" "$SPARSE"; do
  cat "$f" >/dev/null
done

# ── SCENARIO BENCHMARKS ───────────────────────────────────────────────────────

echo -e "${BOLD}${HR}${RESET}"
echo -e "${BOLD}  grep vs krep vs rg — comprehensive benchmark  (${RUNS} runs median)${RESET}"
echo -e "${BOLD}${HR}${RESET}"

# ─── 1. Literal, case-sensitive, count only ───────────────────────────────────
echo -e "\n${CYAN}[1] Literal search, count lines (-c), 50 MB plain text${RESET}"

k=$(measure_ms $RUNS "$KREP" -c "the" "$BIG")
g=$(measure_ms $RUNS "$GREP" -c "the" "$BIG")
r=$(rg_ms $RUNS "$RG" -cF "the" "$BIG")
print_row "common word 'the'" "$k" "$g" "$r"

k=$(measure_ms $RUNS "$KREP" -c "timeout" "$BIG")
g=$(measure_ms $RUNS "$GREP" -c "timeout" "$BIG")
r=$(rg_ms $RUNS "$RG" -cF "timeout" "$BIG")
print_row "medium word 'timeout'" "$k" "$g" "$r"

k=$(measure_ms $RUNS "$KREP" -c "NEEDLE_NOTEXIST_XYZ" "$BIG")
g=$(measure_ms $RUNS "$GREP" -c "NEEDLE_NOTEXIST_XYZ" "$BIG")
r=$(rg_ms $RUNS "$RG" -cF "NEEDLE_NOTEXIST_XYZ" "$BIG")
print_row "no-match pattern" "$k" "$g" "$r"

# ─── 2. Case-insensitive ─────────────────────────────────────────────────────
echo -e "\n${CYAN}[2] Case-insensitive search (-i -c), 50 MB plain text${RESET}"

k=$(measure_ms $RUNS "$KREP" -i -c "THE" "$BIG")
g=$(measure_ms $RUNS "$GREP" -i -c "THE" "$BIG")
r=$(rg_ms $RUNS "$RG" -ciF "THE" "$BIG")
print_row "case-insensitive 'THE'" "$k" "$g" "$r"

k=$(measure_ms $RUNS "$KREP" -i -c "Error" "$BIG")
g=$(measure_ms $RUNS "$GREP" -i -c "Error" "$BIG")
r=$(rg_ms $RUNS "$RG" -ciF "Error" "$BIG")
print_row "case-insensitive 'Error'" "$k" "$g" "$r"

# ─── 3. Fixed string, log file ────────────────────────────────────────────────
echo -e "\n${CYAN}[3] Log file analysis (5 MB, 100K lines)${RESET}"

k=$(measure_ms $RUNS "$KREP" -c "ERROR" "$LOG")
g=$(measure_ms $RUNS "$GREP" -c "ERROR" "$LOG")
r=$(rg_ms $RUNS "$RG" -cF "ERROR" "$LOG")
print_row "count [ERROR] lines" "$k" "$g" "$r"

k=$(measure_ms $RUNS "$KREP" -c "Connection refused" "$LOG")
g=$(measure_ms $RUNS "$GREP" -c "Connection refused" "$LOG")
r=$(rg_ms $RUNS "$RG" -cF "Connection refused" "$LOG")
print_row "multi-word 'Connection refused'" "$k" "$g" "$r"

k=$(measure_ms $RUNS "$KREP" "Authentication failed" "$LOG")
g=$(measure_ms $RUNS "$GREP" "Authentication failed" "$LOG")
r=$(rg_ms $RUNS "$RG" -F "Authentication failed" "$LOG")
print_row "print matching lines" "$k" "$g" "$r"

# ─── 4. Dense matches ────────────────────────────────────────────────────────
echo -e "\n${CYAN}[4] High-density matches (33% lines match)${RESET}"

k=$(measure_ms $RUNS "$KREP" -c "match_" "$DENSE")
g=$(measure_ms $RUNS "$GREP" -c "match_" "$DENSE")
r=$(rg_ms $RUNS "$RG" -cF "match_" "$DENSE")
print_row "dense: count matching" "$k" "$g" "$r"

k=$(measure_ms $RUNS "$KREP" "match_" "$DENSE")
g=$(measure_ms $RUNS "$GREP" "match_" "$DENSE")
r=$(rg_ms $RUNS "$RG" -F "match_" "$DENSE")
print_row "dense: print matching lines" "$k" "$g" "$r"

# ─── 5. Sparse matches ───────────────────────────────────────────────────────
echo -e "\n${CYAN}[5] Sparse matches (1 per 10K lines, 500K lines)${RESET}"

k=$(measure_ms $RUNS "$KREP" -c "NEEDLE_UNIQUE" "$SPARSE")
g=$(measure_ms $RUNS "$GREP" -c "NEEDLE_UNIQUE" "$SPARSE")
r=$(rg_ms $RUNS "$RG" -cF "NEEDLE_UNIQUE" "$SPARSE")
print_row "sparse: count" "$k" "$g" "$r"

# ─── 6. Regex ────────────────────────────────────────────────────────────────
echo -e "\n${CYAN}[6] Regex search (-E -c)${RESET}"

k=$(measure_ms $RUNS "$KREP" -E -c "err(or)?" "$LOG")
g=$(measure_ms $RUNS "$GREP" -E -c "err(or)?" "$LOG")
r=$(rg_ms $RUNS "$RG" -c "err(or)?" "$LOG")
print_row "simple alternation 'err(or)?'" "$k" "$g" "$r"

k=$(measure_ms $RUNS "$KREP" -E -c "[0-9]{4}-[0-9]{2}-[0-9]{2}" "$LOG")
g=$(measure_ms $RUNS "$GREP" -E -c "[0-9]{4}-[0-9]{2}-[0-9]{2}" "$LOG")
r=$(rg_ms $RUNS "$RG" -c "[0-9]{4}-[0-9]{2}-[0-9]{2}" "$LOG")
print_row "date pattern [YYYY-MM-DD]" "$k" "$g" "$r"

k=$(measure_ms $RUNS "$KREP" -E -c "req_id=[0-9]+" "$LOG")
g=$(measure_ms $RUNS "$GREP" -E -c "req_id=[0-9]+" "$LOG")
r=$(rg_ms $RUNS "$RG" -c "req_id=[0-9]+" "$LOG")
print_row "key=value 'req_id=[0-9]+'" "$k" "$g" "$r"

k=$(measure_ms $RUNS "$KREP" -E -c "(ERROR|CRITICAL)" "$LOG")
g=$(measure_ms $RUNS "$GREP" -E -c "(ERROR|CRITICAL)" "$LOG")
r=$(rg_ms $RUNS "$RG" -c "(ERROR|CRITICAL)" "$LOG")
print_row "alternation (ERROR|CRITICAL)" "$k" "$g" "$r"

# ─── 7. Whole word (-w) ──────────────────────────────────────────────────────
echo -e "\n${CYAN}[7] Whole-word search (-w -c)${RESET}"

k=$(measure_ms $RUNS "$KREP" -w -c "the" "$BIG")
g=$(measure_ms $RUNS "$GREP" -w -c "the" "$BIG")
r=$(rg_ms $RUNS "$RG" -cwF "the" "$BIG")
print_row "whole-word 'the'" "$k" "$g" "$r"

k=$(measure_ms $RUNS "$KREP" -w -c "error" "$LOG")
g=$(measure_ms $RUNS "$GREP" -w -c "error" "$LOG")
r=$(rg_ms $RUNS "$RG" -cwF "error" "$LOG")
print_row "whole-word 'error' in log" "$k" "$g" "$r"

# ─── 8. Only matching parts (-o) ─────────────────────────────────────────────
echo -e "\n${CYAN}[8] Print only matching parts (-o)${RESET}"

k=$(measure_ms $RUNS "$KREP" -o "error" "$LOG")
g=$(measure_ms $RUNS "$GREP" -o "error" "$LOG")
r=$(rg_ms $RUNS "$RG" -oF "error" "$LOG")
print_row "literal -o 'error'" "$k" "$g" "$r"

k=$(measure_ms $RUNS "$KREP" -E -o "req_id=[0-9]+" "$LOG")
g=$(measure_ms $RUNS "$GREP" -E -o "req_id=[0-9]+" "$LOG")
r=$(rg_ms $RUNS "$RG" -o "req_id=[0-9]+" "$LOG")
print_row "regex -o 'req_id=[0-9]+'" "$k" "$g" "$r"

# ─── 9. Multiple patterns (-e) ───────────────────────────────────────────────
echo -e "\n${CYAN}[9] Multiple patterns (-e -c)${RESET}"

k=$(measure_ms $RUNS "$KREP" -c -e "ERROR" -e "WARNING" -e "CRITICAL" "$LOG")
g=$(measure_ms $RUNS "$GREP" -c -e "ERROR" -e "WARNING" -e "CRITICAL" "$LOG")
r=$(rg_ms $RUNS "$RG" -cF -e "ERROR" -e "WARNING" -e "CRITICAL" "$LOG")
print_row "3 patterns: ERROR|WARNING|CRITICAL" "$k" "$g" "$r"

k=$(measure_ms $RUNS "$KREP" -c -e "error" -e "failed" -e "timeout" -e "refused" -e "denied" "$LOG")
g=$(measure_ms $RUNS "$GREP" -c -e "error" -e "failed" -e "timeout" -e "refused" -e "denied" "$LOG")
r=$(rg_ms $RUNS "$RG" -cF -e "error" -e "failed" -e "timeout" -e "refused" -e "denied" "$LOG")
print_row "5 patterns in log" "$k" "$g" "$r"

# ─── 10. Multithreaded (-t) vs grep ──────────────────────────────────────────
echo -e "\n${CYAN}[10] Multi-threaded krep (-t 4) vs single-thread grep vs rg (auto-threaded), 50 MB${RESET}"

k=$(measure_ms $RUNS "$KREP" -t 4 -c "the" "$BIG")
g=$(measure_ms $RUNS "$GREP" -c "the" "$BIG")
r=$(rg_ms $RUNS "$RG" -cF "the" "$BIG")
print_row "krep -t4 vs grep vs rg 'the'" "$k" "$g" "$r"

k=$(measure_ms $RUNS "$KREP" -t 4 -c "timeout" "$BIG")
g=$(measure_ms $RUNS "$GREP" -c "timeout" "$BIG")
r=$(rg_ms $RUNS "$RG" -cF "timeout" "$BIG")
print_row "krep -t4 vs grep vs rg 'timeout'" "$k" "$g" "$r"

# ─── 11. Recursive directory search ─────────────────────────────────────────
echo -e "\n${CYAN}[11] Recursive directory search (-r -c)${RESET}"

k=$(measure_ms $RUNS "$KREP" -r -c "search" "$SRCDIR")
g=$(measure_ms $RUNS "$GREP" -r -c "search" "$SRCDIR")
r=$(rg_ms $RUNS "$RG" --no-ignore -cF "search" "$SRCDIR")
print_row "recursive 'search' in src tree" "$k" "$g" "$r"

k=$(measure_ms $RUNS "$KREP" -r -c "TODO\|FIXME" "$SRCDIR")
g=$(measure_ms $RUNS "$GREP" -r -c "TODO\|FIXME" "$SRCDIR")
r=$(rg_ms $RUNS "$RG" --no-ignore -c "TODO|FIXME" "$SRCDIR")
print_row "recursive 'TODO|FIXME'" "$k" "$g" "$r"

# ─── summary ─────────────────────────────────────────────────────────────────
echo -e "\n${BOLD}${HR}${RESET}"
  echo -e "  ${BOLD}Tools:${RESET}  krep=$(${KREP} --version 2>&1 | head -1)   grep=$(${GREP} --version 2>&1 | head -1)   $([ -n "$RG" ] && echo "rg=$(${RG} --version 2>&1 | head -1)" || echo 'rg=not found')"
echo -e "  ${BOLD}Platform:${RESET}  $(uname -ms)   $(sysctl -n machdep.cpu.brand_string 2>/dev/null || uname -p)"
echo -e "${BOLD}${HR}${RESET}"
