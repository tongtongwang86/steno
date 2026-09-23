#!/usr/bin/env python3
"""
Generate reference traces from the real Plover engine.

Runs Plover's own Translator + Formatter headlessly and records, for every
stroke, exactly what it would send to the host: a backspace count and a text
string. The C engine is then diffed against this.

  ./plover_ref.py main.json -o trace.txt --strokes 5000

SPDX-License-Identifier: MIT
"""
import argparse, json, random, sys, os
sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import mkdict

PLOVER_SRC = os.environ.get('PLOVER_SRC', '/tmp/plover_src')
sys.path.insert(0, PLOVER_SRC)

from plover import system                                  # noqa: E402
from plover.registry import registry                       # noqa: E402


class Capture:
    """Plover output sink: records actions instead of typing them."""

    def __init__(self):
        self.ops = []

    def send_backspaces(self, n):
        self.ops.append(('bs', n))

    def send_string(self, s):
        self.ops.append(('str', s))

    def send_key_combination(self, c):
        self.ops.append(('key', c))

    def send_engine_command(self, c, *a):
        self.ops.append(('cmd', c))

    def take(self):
        ops, self.ops = self.ops, []
        return ops


# Running from a source checkout, so the entry-point registry is empty.
# Register the plugins the translator and formatter actually need, from the
# same list setup.cfg declares.
PLUGINS = {
    'system': {'English Stenotype': 'plover.system.english_stenotype'},
    'meta': {
        'attach': 'plover.meta.attach:meta_attach',
        'case': 'plover.meta.case:meta_case',
        'carry_capitalize': 'plover.meta.attach:meta_carry_capitalize',
        'comma': 'plover.meta.punctuation:meta_comma',
        'command': 'plover.meta.command:meta_command',
        'glue': 'plover.meta.glue:meta_glue',
        'if_next_matches': 'plover.meta.conditional:meta_if_next_matches',
        'key_combo': 'plover.meta.key_combo:meta_key_combo',
        'mode': 'plover.meta.mode:meta_mode',
        'retro_case': 'plover.meta.case:meta_retro_case',
        'retro_currency': 'plover.meta.currency:meta_retro_currency',
        'stop': 'plover.meta.punctuation:meta_stop',
        'word_end': 'plover.meta.word_end:meta_word_end',
    },
    'macro': {
        'repeat_last_stroke': 'plover.macro.repeat:last_stroke',
        'retro_delete_space': 'plover.macro.retro:delete_space',
        'retro_insert_space': 'plover.macro.retro:insert_space',
        'retro_toggle_asterisk': 'plover.macro.retro:toggle_asterisk',
        'retrospective_delete_space': 'plover.macro.retro:delete_space',
        'retrospective_insert_space': 'plover.macro.retro:insert_space',
        'retrospective_toggle_asterisk': 'plover.macro.retro:toggle_asterisk',
        'undo': 'plover.macro.undo:undo',
    },
    'dictionary': {
        'json': 'plover.dictionary.json_dict:JsonDictionary',
    },
}


def _load(spec):
    import importlib
    if ':' in spec:
        mod, attr = spec.split(':')
        return getattr(importlib.import_module(mod), attr)
    return importlib.import_module(spec)


def register_all():
    for ptype, entries in PLUGINS.items():
        for name, spec in entries.items():
            try:
                registry.register_plugin(ptype, name, _load(spec))
            except Exception as e:
                print('  warn: could not register %s:%s (%s)'
                      % (ptype, name, e), file=sys.stderr)


def build(dict_path):
    register_all()
    system.setup('English Stenotype')

    from plover.steno_dictionary import StenoDictionaryCollection
    from plover.dictionary.json_dict import JsonDictionary
    from plover.translation import Translator
    from plover.formatting import Formatter

    d = JsonDictionary.load(dict_path)
    dicts = StenoDictionaryCollection([d])

    translator = Translator()
    translator.set_dictionary(dicts)
    formatter = Formatter()
    cap = Capture()
    formatter.set_output(cap)
    translator.add_listener(formatter.format)
    return translator, cap, d


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument('json')
    ap.add_argument('-o', '--output', required=True)
    ap.add_argument('--strokes', type=int, default=5000)
    ap.add_argument('--seed', type=int, default=1)
    ap.add_argument('--max-seq', type=int, default=6,
                    help='max strokes drawn from one dictionary entry')
    args = ap.parse_args()

    translator, cap, d = build(args.json)
    from plover.steno import Stroke

    random.seed(args.seed)
    # Sample stroke sequences from the source JSON rather than the
    # StenoDictionary object, whose iteration API differs across versions.
    with open(args.json) as f:
        raw = json.load(f)
    keys = [tuple(k.split('/')) for k in raw]

    out = []
    n = 0
    while n < args.strokes:
        entry = random.choice(keys)
        if len(entry) > args.max_seq:
            continue
        for s in entry:
            try:
                stroke = Stroke.from_steno(s)
            except Exception:
                break
            translator.translate(stroke)
            ops = cap.take()
            bs = 0
            text = []
            other = []
            for kind, v in ops:
                if kind == 'bs':
                    bs += v
                elif kind == 'str':
                    text.append(v)
                else:
                    other.append('%s:%s' % (kind, v))
            out.append((s, bs, ''.join(text), ';'.join(other)))
            n += 1

    with open(args.output, 'w') as f:
        for s, bs, text, other in out:
            try:
                mask = mkdict.stroke_bits(s)
            except Exception:
                continue
            f.write('%s\t0x%06x\t%d\t%s\t%s\n'
                    % (s, mask, bs, text.replace('\\', '\\\\')
                                        .replace('\t', '\\t')
                                        .replace('\n', '\\n'), other))
    print('wrote %s: %d strokes' % (args.output, len(out)))

    nontrivial = sum(1 for _, bs, _, _ in out if bs)
    withkeys = sum(1 for _, _, _, o in out if o)
    print('  strokes causing backspaces: %d (%.1f%%)'
          % (nontrivial, 100 * nontrivial / len(out)))
    print('  strokes emitting key combos / commands: %d' % withkeys)


if __name__ == '__main__':
    main()
