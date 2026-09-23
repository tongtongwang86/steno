# Devicetree changes

Three edits to your board `.dts`. The first two are correctness, the third is
the one that actually decides whether the display feels live or sluggish.

## 1. LED pin — P1.10

You had `blue_led` on `&gpio0 26`, which is also the 8th entry in
`kscan0`'s `input-gpios`. Corrected to P1.10, which nothing else in the
kscan list uses:

```dts
    leds {
        compatible = "gpio-leds";
        blue_led: led_0 {
            gpios = <&gpio1 10 GPIO_ACTIVE_HIGH>;
        };
    };
```

## 2. I2C bus speed — this is the important one

Your `&i2c0` has `clock-frequency` commented out, so it falls back to the
binding default of **100 kHz**. A full 128x64 frame is 1024 bytes; at 100 kHz
that is roughly **100 ms per repaint**, which makes a live chord display
useless. At 400 kHz it drops to about 25 ms.

```dts
&i2c0 {
	status = "okay";
	compatible = "nordic,nrf-twi";
	clock-frequency = <400000>;   /* I2C_BITRATE_FAST */
	pinctrl-0 = <&i2c0_default>;
	pinctrl-1 = <&i2c0_sleep>;
	pinctrl-names = "default", "sleep";
	...
};
```

I used the literal rather than `I2C_BITRATE_FAST` so you do not have to worry
about which `dt-bindings` include path your ZMK tree wants.

Note the bq274xx shares this bus. It is rated for 400 kHz, so this is safe,
but it does mean a fuel-gauge read can briefly delay a frame. The driver
tolerates that — `display_write` failures are logged and the page stays dirty.

## 3. Display node — no change needed, but check orientation

Your node is correct as written for ZMK's Zephyr 4.1:

```dts
	oled: ssd1306@3c {
	    compatible = "solomon,ssd1306fb";
        reg = <0x3c>;
        width = <128>;
        height = <64>;
        segment-offset = <0>;
        page-offset = <0>;
        com-invdir;
        display-offset = <0>;
        multiplex-ratio = <63>;
        prechargep = <0x22>;
	};
```

`multiplex-ratio = <63>` is right for 64 rows, and the zero offsets are right
for a true SSD1306 (an SH1106 would need `segment-offset = <2>`).

Two things to watch:

- **Orientation.** You have `com-invdir` but `segment-remap` commented out.
  That rotates the panel 180° in one axis only. If your text comes out
  mirrored left-to-right, uncomment `segment-remap`; most 128x64 modules want
  either both or neither.
- **The compatible gets renamed in Zephyr 4.4.** `solomon,ssd1306fb` becomes
  `solomon,ssd1306` (the `fb` suffix was dropped to match other display
  controllers). ZMK is on Zephyr 4.1 today, so keep `ssd1306fb` for now, but
  this will bite on the next Zephyr bump.

## 4. Unrelated, but it will confuse you later: the 61-vs-27 layout mismatch

You include `layouts/common/60percent/ansi.dtsi`, which defines **61** keys,
and point it at a transform that maps **27**.

There is no ZMK build assert for this — I checked every `BUILD_ASSERT` in
`app/src` and `app/include`, and none compares a layout's `keys` length
against its transform's `map` length. So the outcome depends on your keymap:

- Keymap layers with ≤27 bindings: **builds fine, silently wrong.** ZMK sizes
  the keymap from the transform (27), while `keymap_subsystem.c` iterates to
  `layout->keys_len` (61). ZMK Studio will render a 60% ANSI board with 34
  dead keys.
- Keymap layers with 61 bindings: **fails in the C compiler**, not in ZMK,
  with an excess-initializer error naming `zmk_keymap` and no mention of
  physical layouts at all.

For a 27-key steno board you want your own physical layout with 27
`key_physical_attrs` rather than the ANSI include — otherwise Studio and any
future position-map work will be built on a fiction.
