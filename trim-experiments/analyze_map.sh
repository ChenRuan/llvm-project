#!/usr/bin/env bash
# Robust ld map-file parser:
#  * Handles the 2-line format where section-name overflows and the
#    addr/size/path land on the next line.
#  * Skips .debug_* / .note* / .comment / .symtab / .strtab so totals
#    align with what `size` reports in its "text" column.
#
# Usage:
#   analyze_map.sh <mapfile>                    per-archive
#   analyze_map.sh <mapfile> --by-object [arch] per-member [filter]
set -euo pipefail
MAP="${1:?mapfile}"
MODE="${2:-}"
FILTER="${3:-}"

awk -v mode="$MODE" -v filter="$FILTER" '
BEGIN { seen=0; pending="" }
/Linker script and memory map/ { seen=1; next }
!seen { next }

{
  # Section-name-only line (overflow): remember and join with next.
  if (NF == 1 && $1 ~ /^(\.|\*)/) {
    pending = $1
    next
  }

  if (pending != "" && NF >= 3 && $1 ~ /^0x[0-9a-fA-F]+$/ && $2 ~ /^0x[0-9a-fA-F]+$/) {
    sec = pending
    addr = $1
    size = $2
    path = $3
    pending = ""
  } else {
    pending = ""
    if (NF < 4) next
    sec  = $1
    addr = $2
    size = $3
    path = $4
    if (addr !~ /^0x[0-9a-fA-F]+$/) next
    if (size !~ /^0x[0-9a-fA-F]+$/) next
  }

  if (path !~ /\.a\(/) next
  if (sec ~ /^\.debug/)   next
  if (sec ~ /^\.note/)    next
  if (sec ~ /^\.comment/) next
  if (sec ~ /^\.symtab/ || sec ~ /^\.strtab/ || sec ~ /^\.shstrtab/) next

  bytes = strtonum(size)
  match(path, /\(/)
  archive = substr(path, 1, RSTART-1)
  member  = substr(path, RSTART+1, length(path)-RSTART-1)
  n = split(archive, aa, "/")
  abase = aa[n]

  totArch[abase]  += bytes
  totMember[abase "\t" member] += bytes
}

END {
  if (mode == "--by-object") {
    for (k in totMember) {
      split(k, parts, "\t")
      if (filter == "" || parts[1] == filter)
        printf "%12d  %s  %s\n", totMember[k], parts[1], parts[2]
    }
  } else {
    for (a in totArch)
      printf "%12d  %s\n", totArch[a], a
  }
}
' "$MAP" | sort -rn
