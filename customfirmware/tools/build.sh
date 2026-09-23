#!/bin/sh
# Build and verify the dictionary toolchain.
#
#   ./tools/build.sh            compile the host verifier
#   ./tools/build.sh dict       compile main.json -> dict_full.sdic
#   ./tools/build.sh trim       compile with the elimination passes
#   ./tools/build.sh verify     exhaustively verify every entry
#   ./tools/build.sh size       report the Cortex-M4 footprint of the reader
#
# SPDX-License-Identifier: MIT

set -e
cd "$(dirname "$0")/.."

CC=${CC:-gcc}
CROSS=${CROSS:-arm-none-eabi-gcc}
JSON=${JSON:-main.json}

build_host() {
	$CC -std=c11 -Wall -Wextra -O2 -o /tmp/test_sdic tools/test_sdic.c dict/sdic.c
}

case "${1:-host}" in
host)
	build_host
	echo "built /tmp/test_sdic"
	;;
dict)
	python3 tools/mkdict.py "$JSON" -o dict_full.sdic --max-rules 6000
	;;
trim)
	python3 tools/mkdict.py "$JSON" -o dict_trim.sdic --max-rules 6000 \
		--drop-derivable --drop-generated
	;;
verify)
	build_host
	for img in dict_full.sdic dict_trim.sdic; do
		[ -f "$img" ] || continue
		case "$img" in
		*trim*) FLAGS="--drop-derivable --drop-generated" ;;
		*)      FLAGS="" ;;
		esac
		echo "=== $img ==="
		python3 tools/dump_pairs.py "$JSON" -o /tmp/pairs.bin $FLAGS >/dev/null
		/tmp/test_sdic "$img" /tmp/pairs.bin
	done
	;;
size)
	$CROSS -std=c11 -Os -mcpu=cortex-m4 -mthumb \
		-ffunction-sections -fdata-sections \
		-c dict/sdic.c -o /tmp/sdic.o
	arm-none-eabi-size -A /tmp/sdic.o | awk '
		/^\.text/{t+=$2} /^\.rodata/{r+=$2} /^\.bss/{b+=$2} /^\.data/{d+=$2}
		END{printf "flash %d B, static RAM %d B\n", t+r, b+d}'
	$CROSS -std=c11 -Os -mcpu=cortex-m4 -mthumb -fstack-usage \
		-c dict/sdic.c -o /tmp/sdic.o
	echo "peak stack:"
	sort -t"$(printf '\t')" -k2 -rn /tmp/sdic.su | head -3
	rm -f /tmp/sdic.su
	;;
*)
	echo "usage: $0 {host|dict|trim|verify|size}" >&2
	exit 2
	;;
esac
