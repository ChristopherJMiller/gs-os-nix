#!/usr/bin/env bash
# Report ECP5 area per RTL module.
#
# ARCHITECTURE.md carries a gate budget for the card. That budget is only useful if
# it tracks reality, so this prints measured LUT4/FF/carry counts per module. Run it
# whenever a module lands or grows, and update the table in ARCHITECTURE.md.
#
# -noabc9 keeps this fast and deterministic; absolute LUT counts shift slightly with
# ABC9 enabled, but the budget only needs to be right to ~10%.
set -euo pipefail

RTL="rtl/sha256_core.v rtl/sha256.v rtl/storepath.v rtl/keycam.v rtl/a2nix_card.v"
TOPS=("$@")
if [ ${#TOPS[@]} -eq 0 ]; then
    TOPS=(sha256_core sha256 storepath keycam a2nix_card)
fi

# Pull the "including submodules" totals out of a yosys stat report.
count() {
    # $1 = log text, $2 = cell type
    printf '%s\n' "$1" | grep -E "^[[:space:]]+[0-9]+[[:space:]]+$2\$" |
        tail -1 | tr -s ' ' | cut -d' ' -f2
}

printf '%-16s %8s %8s %8s %8s %8s\n' module LUT4 FF CCU2C PFUMX DPR16X4
printf '%-16s %8s %8s %8s %8s %8s\n' ---------------- -------- -------- -------- -------- --------

for top in "${TOPS[@]}"; do
    log=$(yosys -p "read_verilog $RTL; synth_ecp5 -top $top -noabc9; stat" 2>&1 || true)
    if [ -z "$(count "$log" LUT4)" ]; then
        printf '%-16s %8s  (synthesis produced no stats -- check the module name)\n' \
            "$top" "ERR"
        continue
    fi
    printf '%-16s %8s %8s %8s %8s %8s\n' \
        "$top" \
        "$(count "$log" LUT4)" \
        "$(count "$log" TRELLIS_FF)" \
        "$(count "$log" CCU2C)" \
        "$(count "$log" PFUMX)" \
        "$(count "$log" TRELLIS_DPR16X4)"
done
