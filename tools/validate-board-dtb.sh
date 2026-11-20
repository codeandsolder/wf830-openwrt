#!/usr/bin/env bash
# SPDX-License-Identifier: GPL-2.0-only
# Validate the complete hardware-backed P4 DTB and vendor binding set.
set -euo pipefail

if (( $# != 2 )); then
    echo "usage: $0 path/to/gct-wf830-p4.dtb path/to/linux-6.12-git" >&2
    exit 2
fi

dtb="$(realpath "$1")"
kernel="$(realpath "$2")"
repo="$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")/.." && pwd)"
bindings="$repo/target/linux/gdm7243/files/Documentation/devicetree/bindings"
out="$(mktemp -d "$(dirname -- "$dtb")/.wf830-schema.XXXXXXXX")"
trap 'rm -rf -- "$out"' EXIT

mapfile -t vendor_schemas < <(find "$bindings" -type f -name '*.yaml' | sort)
(( ${#vendor_schemas[@]} == 7 )) || {
    echo "Expected all seven GDM7243 binding schemas" >&2
    exit 1
}
vendor_schemas+=("$kernel/Documentation/devicetree/bindings/pinctrl/pinctrl.yaml")

# These dtschema tools can report semantic errors while returning zero.
# Treat any schema-generation or board validation diagnostic as failure.
uvx --from dtschema dt-mk-schema --useronly -o "$out/schemas.yaml" \
    "${vendor_schemas[@]}" > "$out/schema.log" 2>&1
if [[ -s "$out/schema.log" ]]; then
    cat "$out/schema.log" >&2
    exit 1
fi

uvx --from dtschema dt-validate -s "$out/schemas.yaml" \
    -l gdm7243 "$dtb" > "$out/validation.log" 2>&1
if [[ -s "$out/validation.log" ]]; then
    cat "$out/validation.log" >&2
    exit 1
fi

# Pin down the exact observed P4 RAM/MMIO/IRQ reservations.
[[ "$(fdtget -t x "$dtb" /memory@d0000000 reg)" == "d0000000 5000000" ]]
[[ "$(fdtget -t x "$dtb" /reserved-memory/memory@d4800000 reg)" == "d4800000 800000" ]]
[[ "$(fdtget -t x "$dtb" /timer@fc006c40 interrupts)" == "12" ]]
[[ "$(fdtget -t x "$dtb" /gpio@fc002080 interrupts)" == "a 1d" ]]
[[ "$(fdtget -t x "$dtb" /serial@fc007500 interrupts)" == "2 3" ]]
[[ "$(fdtget -t x "$dtb" /serial@fc007540 interrupts)" == "15 16" ]]
[[ "$(fdtget -t x "$dtb" /serial@fc007580 interrupts)" == "21 22" ]]
[[ "$(fdtget -t s "$dtb" /serial@fc007500 status)" == "okay" ]]
[[ "$(fdtget -t s "$dtb" /serial@fc007540 status)" == "disabled" ]]
[[ "$(fdtget -t s "$dtb" /serial@fc007580 status)" == "disabled" ]]
[[ "$(fdtget -t s "$dtb" /chosen stdout-path)" == "serial0:921600n8" ]]
args="$(fdtget -t s "$dtb" /chosen bootargs)"
[[ "$args" == *"rdinit=/init"* && "$args" == *"ttyGDM0"* ]]
[[ "$args" != *"ubi.mtd"* && "$args" != *"mtdparts"* ]]

echo "Validated seven GDM7243 schemas, the P4 physical map and RAM-only console"
