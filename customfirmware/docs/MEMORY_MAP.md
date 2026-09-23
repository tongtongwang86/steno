# Memory map

Settled against **measured builds**, not estimates. Everything below comes from
linking real firmware for your board: ZMK on Zephyr 4.1, nRF52840, BLE + USB,
bq274xx fuel gauge, 27-key direct kscan, and the OLED driver.

## Measured firmware

Three real configurations of the shipped firmware:

| build | flash | RAM |
|---|---|---|
| no display | 196,508 B | 46,580 B |
| **+ framebuffer display (shipped)** | **198,872 B** | **48,956 B** |
| + dictionary reader wired in | 199,224 B | 48,956 B |

The shipped firmware sets `CONFIG_STENO_DISPLAY=y` and contains **zero** LVGL
symbols. Separately, as a counterfactual only, LVGL was built once in a
throwaway tree to price the road not taken:

| counterfactual | flash | RAM |
|---|---|---|
| `CONFIG_ZMK_DISPLAY` (LVGL) instead | 314,880 B | 55,764 B |

Three things fall out of this:

**The earlier 250–350 KB estimate was wrong, and wrong in the helpful
direction.** A complete ZMK BLE+USB build for this board is **194 KB**, not
350. That is ~150 KB more dictionary than I had been budgeting for.

**The display driver costs 2,364 B of flash and 2,376 B of RAM.** That is the
whole thing — framebuffer, 5×7 font, steno key layout, ZMK event listeners.

**LVGL costs 118,372 B more — 50× the framebuffer driver.** It does not even
fit: with `CONFIG_ZMK_DISPLAY=y` the link fails with *"region FLASH overflowed
by 52,736 bytes"* against a 256 KB code partition. 118 KB is about **21,000
dictionary entries**. The no-LVGL decision was worth more than it looked.

## The map

```
0x000000  +---------------------------+
          | code_partition    272 KB  |  199 KB used (71%), 73 KB headroom
0x044000  +---------------------------+
          | dict_partition    720 KB  |  694 KB image, 26 KB spare
0x0F8000  +---------------------------+
          | storage_partition  32 KB  |  8 NVS sectors x 4 KB
0x100000  +---------------------------+
```

Exactly 1024 KB, no bootloader, no MBR. Confirmed against the generated
devicetree of a successful build.

```dts
&flash0 {
	partitions {
		compatible = "fixed-partitions";
		#address-cells = <1>;
		#size-cells = <1>;

		code_partition: partition@0 {
			reg = <0x00000000 0x00044000>;   /* 272K */
		};
		dict_partition: partition@44000 {
			reg = <0x00044000 0x000b4000>;   /* 720K */
		};
		storage_partition: partition@f8000 {
			reg = <0x000f8000 0x00008000>;   /* 32K */
		};
	};
};
```

### Why these sizes

**code 272 KB.** 199 KB measured leaves 73 KB for the steno engine. That is the
one number here still unmeasured — the engine does not exist yet. For scale,
the dictionary reader is 1.1 KB and Javelin's *entire* core engine, with every
feature enabled and before dead-code elimination, is ~165 KB of `.text`; a
lookup + segmentation + spacing + orthography engine should land well inside
73 KB. If it does not, move the boundary: every 4 KB taken from the dictionary
costs ~700 entries.

**dict 720 KB.** The trimmed image is 694 KB, so 26 KB spare. The full
147k-entry image is 796 KB and does **not** fit — you would need to take 76 KB
from code, leaving only 199 KB there and no engine headroom. Use
`--drop-derivable`, which is what the orthography engine is for anyway.

**storage 32 KB.** ZMK defaults to `CONFIG_SETTINGS_NVS_SECTOR_COUNT=8` at the
4 KB nRF52840 page size. Exactly 32 KB — do not shrink it or BLE bonds stop
persisting across resets.

### Alignment

Every boundary is 4 KB aligned, which the nRF52840's flash page size requires
for erase. 0x44000 and 0xF8000 both are.

## Flashing with a Black Magic Probe

### Firmware

```sh
arm-none-eabi-gdb /tmp/b_final/zephyr/zmk.elf \
  -ex 'target extended-remote /dev/ttyACM0' \
  -ex 'monitor swdp_scan' \
  -ex 'attach 1' \
  -ex 'load' \
  -ex 'kill' -ex 'quit'
```

### Dictionary, independently

The image is position independent, so it is just bytes at an address. Wrap it
in an ELF so `load` handles the flash erase for you:

```sh
arm-none-eabi-objcopy -I binary -O elf32-littlearm -B arm \
  --rename-section .data=.dict,alloc,load,readonly,data,contents \
  --change-section-address .data=0x44000 \
  dict_trim.sdic dict.elf

arm-none-eabi-gdb dict.elf \
  -ex 'target extended-remote /dev/ttyACM0' \
  -ex 'monitor swdp_scan' -ex 'attach 1' -ex 'load' -ex 'kill' -ex 'quit'
```

Verified: that produces `.dict` at VMA/LMA `0x00044000`, 710,956 bytes, which
matches `dict_partition` and the image size exactly.

Note `--change-section-address` takes the **original** section name (`.data`),
not the renamed one — objcopy evaluates the address change before the rename
and silently warns "never used" if you give it `.dict`.

Updating the dictionary does not touch the firmware, and vice versa.

## What changes without a bootloader

- **No UF2.** `CONFIG_BUILD_OUTPUT_UF2` is off; there is no mass-storage
  device and no double-tap-reset. Every flash is over SWD.
- **The application owns the vector table at 0x0.** `code_partition` starts at
  0, `CONFIG_USE_DT_CODE_PARTITION=y` links it there.
- **A bad flash is recovered with the probe, not the bootloader.** Keep the
  SWD pads reachable. This is the real cost of the 48 KB you reclaimed.
- ZMK's `imply RETENTION_BOOT_MODE` is still set by the board Kconfig. It is
  harmless with no bootloader to signal, but "reset to bootloader" behaviours
  in a keymap will do nothing.

## Runtime wiring

`dict/steno_dict.c` finds the image from devicetree and opens it in place —
internal flash is memory mapped at 0, so the partition offset is the address
and there is no copy and no flash driver:

```c
#define DICT_NODE DT_NODELABEL(dict_partition)
BUILD_ASSERT(DT_NODE_EXISTS(DICT_NODE), "no dict_partition in devicetree");
BUILD_ASSERT(DT_REG_ADDR(DT_NODELABEL(flash0)) == 0,
	     "flash0 is not mapped at 0; dictionary addressing assumes it is");

sdic_open(&dict, (const void *)(uintptr_t)DT_REG_ADDR(DICT_NODE),
	  DT_REG_SIZE(DICT_NODE));
```

Both asserts are compile-time, so moving the partition or changing the flash
base breaks the build rather than producing a keyboard that reads garbage.

A missing or corrupt image is **not** a boot failure. `sdic_open` validates
magic, version and size, logs, and leaves `steno_dict_available()` false — the
keyboard still enumerates over USB and BLE and still reports battery, so a
board with no dictionary flashed is diagnosable rather than dead.

## Build

```sh
west build -s app -b steno60/nrf52840/zmk -d build -- \
  -DZEPHYR_TOOLCHAIN_VARIANT=cross-compile \
  -DCROSS_COMPILE=/usr/bin/arm-none-eabi-
```

The board lives at `app/boards/stenolab/steno60/` (Zephyr hardware model v2 —
`app/boards/arm/` no longer exists).

Custom sources are compiled into the `app` target rather than as a
`zephyr_library()`. A board-level `zephyr_library()` does **not** inherit ZMK's
include directories, so `<zmk/event_manager.h>` fails to resolve.

## Still unmeasured

The steno engine. Every other figure here is from a linker map. Treat the 73 KB
of code headroom as the budget to build against, and re-measure as soon as
there is an engine to measure — the partition boundary is one DTS line and a
reflash, since the dictionary image does not care where it lives.
