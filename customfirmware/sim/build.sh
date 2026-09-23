#!/bin/sh
# Offline preview harness. Not part of the firmware build.
#
#   ./build.sh          build and render the display to stdout as ASCII art
#   ./build.sh clean    remove the binary and the generated stub headers
#
# SPDX-License-Identifier: MIT

set -e
cd "$(dirname "$0")"

# Every Zephyr/ZMK header steno_display.c includes. Each is generated as a
# two-line shim onto stubs.h, so the real driver compiles unmodified.
STUBS="
zephyr/kernel.h
zephyr/device.h
zephyr/drivers/display.h
zephyr/logging/log.h
zephyr/sys/util.h
zmk/event_manager.h
zmk/activity.h
zmk/battery.h
zmk/ble.h
zmk/endpoints.h
zmk/endpoints_types.h
zmk/events/activity_state_changed.h
zmk/events/battery_state_changed.h
zmk/events/ble_active_profile_changed.h
zmk/events/endpoint_changed.h
"

if [ "$1" = "clean" ]; then
	rm -rf sim zephyr zmk
	echo "cleaned"
	exit 0
fi

for h in $STUBS; do
	mkdir -p "$(dirname "$h")"
	printf '#pragma once\n#include "stubs.h"\n' > "$h"
done

${CC:-gcc} -std=c11 -Wall -I. -o sim main.c
./sim
