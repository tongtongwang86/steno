# Steno engine

Stroke segmentation and output formatting. Pure C11, no RTOS or ZMK
dependency, so it can be replayed against reference traces on a host.

## Verification

The engine is diffed against **the real Plover engine**, not against my
expectations. `tools/plover_ref.py` runs Plover's own `Translator` and
`Formatter` headlessly and records the exact `(backspaces, text)` it would
send the host for every stroke. `tools/test_engine.c` replays the same
strokes through the C engine — using the real `dict_full.sdic` image — and
compares every action.

```
dict_full.sdic   20002 strokes, 19941 exact (99.7%)
dict_full.sdic   undo trace,     7975 exact (99.7%)
dict_ship.sdic   20002 strokes, 19417 exact (97.1%)
```

26.6% of strokes involve backspaces, from retroactive re-segmentation, so
this is exercising the hard path rather than simple appends. The undo trace
is the same replay with `*` strokes mixed in.

The gap between the two images is **trimming, not engine behaviour**:
`dict_ship.sdic` drops ~8,700 entries to fit the 752 KB partition, and a
dropped entry becomes an untranslated stroke. Run against the full image the
engine is at 99.7%.

**The residual 0.3% is one known, unclosable divergence**, not a bug list:
Plover's `_add_suffix` ranks candidate spellings against
`american_english_words.txt` (338,882 words, 4.3 MB), which cannot be
embedded. Forcing Plover onto its own no-word-list fallback path — the same
path this engine implements — puts the achievable ceiling at 99.9%, so the
word list is worth about 0.2%.

## Footprint

`arm-none-eabi-gcc -Os -mcpu=cortex-m4`:

| | |
|---|---|
| flash — `steno_engine.c` | 3,068 B |
| flash — `steno_ortho.c` | 3,198 B |
| flash — `steno_combo.c` | 1,165 B |
| static RAM | **0 B** |
| RAM per engine instance | 8,472 B (4,928 B of it the undo ring) |
| peak stack (`steno_engine_stroke`) | 896 B |

Whole firmware, linked: **211,688 B of the 240 KB code partition (86.1%)**,
62,356 B of 256 KB RAM. For comparison, the same display built on LVGL came
to 314,880 B and did not fit at all.

## Segmentation

Follows Plover's `Translator.translate_stroke` exactly:

1. **Longest multi-stroke match.** Walk back over retained translations while
   the accumulated stroke count stays within the dictionary's longest key,
   then try candidates longest-first. The first hit wins and *replaces* the
   translations it consumed — this is what produces backspaces.
2. **Single-stroke match**, if no multi-stroke match.
3. **Implicit suffix folding.** If the stroke carries one of Plover's
   `SUFFIX_KEYS` (`-Z`, `-D`, `-S`, `-G`) and removing that key yields a
   match, the suffix's own translation is appended as `main + " " + suffix`.
4. **Untranslated**, rendered as raw steno (`TKPW-G`).

Stroke bit order is `#STKPWHRAO*EUFRPBLGTSDZ`, byte-for-byte Plover's `KEYS`
tuple — verified against the running system, not assumed.

## Output diffing

After each stroke the retained window is re-rendered from scratch and diffed
against what was last emitted: common prefix length gives the backspace count
and the tail gives the new text. Simple, and it makes re-segmentation fall out
for free rather than needing separate undo bookkeeping.

### The window-retirement bug worth remembering

Whether a segment gets a leading space depends on whether the *previous* one
ended attached. So retiring the oldest segment silently changed how the next
one rendered, the diff lost its anchor at offset 0, and the engine re-emitted
the entire window — 151 backspaces where Plover sent none.

The fix carries formatter state (`space_next`, `case_next`, `last_glue`)
across the window head, snapshotting it at each segment boundary during
rendering. Retirement is now also bounded by the render buffer rather than
only the segment count, because a run of long translations would otherwise
overflow it and silently corrupt every later diff.

The test for "is this actually fixed" is that results are now **identical at
window sizes 16, 24 and 64**. Before the fix, 64 scored 3.3%.

## Orthography — the remaining 2.2%, and a hard constraint

Every remaining mismatch is a suffix join Plover spells differently:

```
PAOUPBG   want ' puning'    got ' puneing'     (drop silent e)
TPHRUBG   want ' flubbing'  got ' flubing'     (double final consonant)
TEU       want bs=5         got bs=4           (knock-on backspace count)
```

Plover applies 38 ordered regex rules, of which the last two do most of the
work:

```
^(.+[bcdfghjklmnpqrstuvwxz])e \^ ([aeiouy].*)$                      -> \1\2
^(.*(?:[bcdfghjklmnprstvwxyz]|qu)[aeiou])([bcdfgklmnprtvz]) \^ ([aeiouy].*)$ -> \1\2\2\3
```

**The constraint: `_add_suffix` ranks candidate spellings against
`american_english_words.txt` — 338,882 words, 4.3 MB.** That cannot be
embedded; it is six times the entire dictionary budget. So byte-exact parity
with Plover on orthography is not achievable on this part, and it would be
dishonest to claim otherwise.

What *is* achievable is Plover's own fallback path. When no candidate appears
in the word list, Plover applies the ordered rules and takes the first match —
no ranking involved. Implementing that path should close most of the 2.2%,
and the residue will be words where the list would have disambiguated between
two plausible spellings.

Two implementation options, neither needing a regex engine:

- **Hand-compile each rule to a small C predicate.** ~38 short functions,
  auditable, no interpreter, probably 3–5 KB.
- **A minimal regex subset.** More general, but rule 7 uses negative
  lookbehind (`(?<![gin]a)`), which is real work to support.

The first is the better trade here.

## Key combos

`{#Return}`, `{#control(c)}`, `{#shift(a b)}` — Plover's X11 keysym syntax,
with nesting and sequences — are parsed into `(mods, HID usage)` pairs and
tapped through ZMK's HID layer after the stroke's text.

They are collected from **the new segment's own translation**, not from the
rendered window. This matters: the window is re-rendered from scratch on
every stroke, so harvesting during rendering would re-fire every combo in
the window on every stroke.

Two honest limitations:

- **A combo cannot be undone.** `*` retracts text by sending backspaces;
  there is no way to un-send a Ctrl-C. Plover has the same limitation.
- **Unknown keysyms produce nothing**, rather than a wrong key. The table
  covers what a keyboard needs (a–z, 0–9, F1–F12, arrows, navigation,
  punctuation, the eight modifiers); it is not all of X11.

main.json uses exactly one combo (`W-FP` → `{#BackSpace}`), so this is not
for the main dictionary — it is what makes a *user* dictionary worth having.
`dict/user.json` is a worked example: Enter, Tab, Shift-Tab, Escape, the
clipboard shortcuts, arrows and F5.

Entries from any dictionary after the first are **pinned against trimming**.
Without that, `--max-bytes` would rank a user's own `{#Return}` brief by word
frequency — it has no frequency — and could silently drop the binding they
deliberately added.

## Not implemented yet

Beyond orthography:

- **Prefix strokes** — Plover's `_lookup_with_prefix` handles entries keyed
  on a leading empty stroke. main.json has **0** of these.
- **Retro transforms** — `{:retro_case:}`, `{:retro_currency:}`. Swallowed
  rather than typed as literal braces, which is the right failure mode but
  not the right behaviour. main.json has **1**.
- **Engine commands** — `{PLOVER:...}`. Parsed and discarded. main.json has
  7 of these and none of them mean anything on a standalone keyboard.
- **Number/fingerspelling generation** — the `--drop-generated` compiler pass
  assumes these get generated at runtime. It currently drops only 288 entries,
  so this is low priority.

Note the compiler's `--drop-derivable` removes 26,099 entries on the
assumption orthography regenerates them. **Do not ship a `--drop-derivable`
image until orthography is implemented**, or those words are simply gone. The
full 796 KB image has no such dependency.

## Reproducing

```sh
# one-time: Plover's core, headless
git clone --depth 1 https://github.com/openstenoproject/plover /tmp/plover_src
pip install appdirs plover_stroke rtf_tokenize

# dictionaries
./tools/build.sh dict          # dict_full.sdic, the reference image
./tools/build.sh ship          # dict_ship.sdic, trimmed to the partition

# reference trace + replay
python3 tools/plover_ref.py main.json -o /tmp/trace.txt --strokes 20000
gcc -std=c11 -O2 -o test_engine tools/test_engine.c \
	engine/steno_engine.c engine/steno_ortho.c engine/steno_combo.c \
	dict/sdic.c -Iengine -Idict
./test_engine dict_full.sdic /tmp/trace.txt -v

# firmware. Note the full board target: `-b steno` alone silently falls
# back to a stub devicetree and fails much later with a confusing
# __NVIC_PRIO_BITS error.
cd ../zmkws/zmk/app
west build -d /tmp/b -b steno/nrf52840/zmk -- -DBOARD_ROOT=/path/to/zmkcfg
```

Plover's plugin registry is populated from installed entry points, so running
from a source checkout finds nothing; `plover_ref.py` registers the needed
system, meta and macro plugins by hand from the same list `setup.cfg`
declares.
