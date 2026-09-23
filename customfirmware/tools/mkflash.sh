#!/bin/sh
# Produce the two things Black Magic Probe needs to `load`:
#
#   flash/steno-firmware.elf     straight from the west build
#   flash/steno-dictionary.elf   the dictionary image wrapped so GDB puts
#                                it at the dict partition base
#
# Usage:  ./tools/mkflash.sh [build-dir] [dict.sdic]
#
# These are the two names flash/flash.sh expects, so:
#
#   ./tools/mkflash.sh /tmp/b dict_ship.sdic
#   ./flash/flash.sh
#
# SPDX-License-Identifier: MIT

set -e
cd "$(dirname "$0")/.."

BUILD=${1:-/tmp/bcombo}
DICT=${2:-dict_ship.sdic}

# Must match dict_partition in steno_nrf52840_zmk.dts.
DICT_BASE=0x0003c000
DICT_SIZE=770048   # 752K

OBJCOPY=${OBJCOPY:-arm-none-eabi-objcopy}

[ -f "$BUILD/zephyr/zmk.elf" ] || { echo "no firmware at $BUILD" >&2; exit 1; }
[ -f "$DICT" ] || { echo "no dictionary at $DICT" >&2; exit 1; }

bytes=$(wc -c < "$DICT")
if [ "$bytes" -gt "$DICT_SIZE" ]; then
	echo "$DICT is $bytes B, over the ${DICT_SIZE} B partition" >&2
	exit 1
fi

mkdir -p flash
cp "$BUILD/zephyr/zmk.elf" flash/steno-firmware.elf

# objcopy evaluates --change-section-address against the section name the
# input *currently* has, which for a binary input is always `.data`. Renaming
# in the same invocation is fine, but the address must be keyed on `.data`
# or it is silently ignored and the image lands at 0.
$OBJCOPY -I binary -O elf32-littlearm -B arm \
	--rename-section .data=.dict,alloc,load,readonly,data,contents \
	--change-section-address .data=$DICT_BASE \
	"$DICT" flash/steno-dictionary.elf

code_used=$(arm-none-eabi-size flash/steno-firmware.elf | awk 'NR==2{print $1+$2}')
echo "firmware   $code_used B of 245760 B code partition"
echo "dictionary $bytes B of $DICT_SIZE B dict partition, based at $DICT_BASE"

# Cheap guard against the objcopy address silently not taking effect.
arm-none-eabi-objdump -h flash/steno-dictionary.elf | grep '\.dict'
arm-none-eabi-objdump -h flash/steno-dictionary.elf | grep -q " $(printf %08x $DICT_BASE) " || {
	echo "error: .dict did not land at $DICT_BASE" >&2
	exit 1
}
