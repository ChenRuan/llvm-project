#!/usr/bin/env bash
# Try every candidate LLVM archive in isolation: drop it from the baseline
# list, attempt to link, report size if OK or failure reason if not.
# This is a quick-and-dirty ablation study.
set -u
TRIM_ROOT="$(cd "$(dirname "$0")" && pwd)"
BASE="$TRIM_ROOT/archives_baseline.txt"
OUT="$TRIM_ROOT/out"
LOG="$OUT/ablation.log"
mkdir -p "$OUT"
: > "$LOG"

CANDIDATES=(
  libLLVMAArch64AsmParser.a
  libLLVMAArch64Utils.a
  libLLVMAsmPrinter.a
  libLLVMBinaryFormat.a
  libLLVMBitReader.a
  libLLVMBitstreamReader.a
  libLLVMBitWriter.a
  libLLVMCFGuard.a
  libLLVMDebugInfoCodeView.a
  libLLVMDebugInfoDWARF.a
  libLLVMDemangle.a
  libLLVMExecutionEngine.a
  libLLVMGlobalISel.a
  libLLVMJITLink.a
  libLLVMLinker.a
  libLLVMMCParser.a
  libLLVMObject.a
  libLLVMOrcShared.a
  libLLVMOrcTargetProcess.a
  libLLVMProfileData.a
  libLLVMRemarks.a
  libLLVMRuntimeDyld.a
  libLLVMScalarOpts.a
  libLLVMTarget.a
  libLLVMTextAPI.a
  libLLVMipo.a
)

printf "%-35s %-10s %s\n" "DROPPED" "TEXT_Δ" "STATUS" | tee -a "$LOG"
BASE_SIZE=$(size "$OUT/baseline" 2>/dev/null | awk 'NR==2{print $1}')
echo "baseline text = $BASE_SIZE" | tee -a "$LOG"

for drop in "${CANDIDATES[@]}"; do
  tmp_list=$(mktemp)
  grep -v "^${drop}\$" "$BASE" > "$tmp_list"
  tag="$(basename "$drop" .a)"
  out_bin="$OUT/a_${tag}"
  if "$TRIM_ROOT/link_static_add_int.sh" "a_${tag}" "$tmp_list" >"$out_bin.buildlog" 2>&1; then
    new_size=$(size "$out_bin" 2>/dev/null | awk 'NR==2{print $1}')
    if "$out_bin" 2>/dev/null | head -1 | grep -q 'inc(4) is 5'; then
      delta=$((new_size - BASE_SIZE))
      printf "%-35s %+10d %s\n" "$drop" "$delta" "OK (text=$new_size, runs)" | tee -a "$LOG"
    else
      printf "%-35s %-10s %s\n" "$drop" "?" "LINK OK but failed to run" | tee -a "$LOG"
    fi
  else
    first_err=$(grep -m1 'undefined reference' "$out_bin.buildlog" | sed 's/.*undefined reference to //' | head -c 120)
    printf "%-35s %-10s %s\n" "$drop" "-" "LINK FAIL: ${first_err}" | tee -a "$LOG"
  fi
  rm -f "$tmp_list"
done

echo "full log: $LOG"
