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
strokes replayed: 20002
exact matches:    19570 (97.8%)
  backspace mismatches: 249
  text mismatches:      245
engine: 102254 lookups, 784 untranslated strokes
```

26.6% of strokes involve backspaces, from retroactive re-segmentation, so
this is exercising the hard path rather than simple appends.

**Every remaining failure is the same missing feature: orthography.** See
below.

## Footprint

`arm-none-eabi-gcc -Os -mcpu=cortex-m4`:

| | |
|---|---|
| flash (`.text` + `.rodata`) | **1,951 B** |
| static RAM | **0 B** |
| RAM per engine instance | 3,496 B |
| peak stack (`steno_engine_stroke`) | 896 B |

With the dictionary reader that is ~3.1 KB of flash against 73 KB of
headroom.

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

## Not implemented yet

Beyond orthography:

- **Macros** — `=undo`, `=retro_insert_space`, `repeat_last_stroke`. The
  `*` key undo is the notable one; users expect it.
- **Prefix strokes** — Plover's `_lookup_with_prefix` handles entries keyed
  on a leading empty stroke, for attaching to the start of a word.
- **Retro transforms** — `{:retro_case:}`, `{:retro_currency:}`. Currently
  swallowed rather than typed as literal braces, which is the right failure
  mode but not the right behaviour.
- **Key combos and engine commands** — `{#Return}`, `{PLOVER:...}`. Parsed
  and discarded; they need a path to ZMK's HID layer.
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

# reference trace + replay
python3 tools/plover_ref.py main.json -o /tmp/trace.txt --strokes 20000
gcc -std=c11 -O2 -o test_engine tools/test_engine.c engine/steno_engine.c dict/sdic.c
./test_engine dict_full.sdic /tmp/trace.txt -v
```

Plover's plugin registry is populated from installed entry points, so running
from a source checkout finds nothing; `plover_ref.py` registers the needed
system, meta and macro plugins by hand from the same list `setup.cfg`
declares.
