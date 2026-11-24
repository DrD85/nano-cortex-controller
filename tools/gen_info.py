#!/usr/bin/env python3
"""Generates main/fx_info.h/.c: what the FX editor's info page tells about a model.

It is the Nano Cortex Editor's large effect view (FX_INFO / FX_INFO_MORE and the players' sounds in index.html), so
the controller and the editor tell the same: the device a model is based on (or classic devices of its kind), when
and by whom it was made, its story, where it is heard, starting points and players' sounds, tips.

- With the editor's folder next to this project (and Google Chrome, or CHROME=<path>) the editor's page is run and
  asked for its notes; every value of a starting point comes as the Nano's 0-1 of its control (the editor's own
  conversion, with its units and skews) and as the text the editor shows ("Gain 2.5 · Tone 4.5"). It is kept as
  tools/editor_info.json, so the firmware builds without the editor. (Without Chrome: open the editor's page in any
  browser, run EXPORT_SCRIPT's body in its console and save what it puts into the title.)
- From tools/editor_info.json the C table is written, checked against tools/editor_models.json: a model or a
  control the model table does not have (or has under another number) stops the run.

The texts are the editor's public ones; pictures are not part of this (the info page shows the own build's pedal
picture from the pedal view's file, see tools/gen_pedalboard.py).

Run from the project folder:  python3 tools/gen_info.py
"""
import html
import json
import os
import pathlib
import re
import subprocess
import sys
import tempfile

ROOT = pathlib.Path(__file__).resolve().parent.parent
MODELS = json.loads((ROOT / 'tools' / 'editor_models.json').read_text())
INFO_JSON = ROOT / 'tools' / 'editor_info.json'
MAX_SOUNDS = 12   # starting points + players' sounds of one model (INFO_MAX_SOUNDS in main/ui.c)

EXPORT_SCRIPT = """<script>
setTimeout(() => {
  const out = [], seen = {};
  for (const key of FX_SLOT_KEYS) for (const m of state.blocks[key].models) {
    const type = modelType(m);
    if (!type || seen[type]) continue;
    const info = fxInfoOf(m.name);
    if (!info) continue;
    seen[type] = true;
    const block = { params: structuredClone(m.params) };
    const conv = list => (list || []).map(st => ({ name: st.name, why: st.why || '', asked: Object.keys(st.values || {}).length,
      text: fxStartText(block, st),
      values: fxStartParams(block, st).map(({ param, value }) =>
        [param.index, param.name, Number(paramValueToNormalized(Object.assign({}, param, { value })).toFixed(4))]) }));
    out.push({ type, name: m.name, cat: info.cat || '', basedOn: info.basedOn || '', like: info.like || '', made: info.made || '',
      text: info.text || '', heard: info.heard || '', photo: !!info.photo, rich: !!info.rich, tips: info.tips || [],
      starts: conv(info.starts), players: conv(fxArtistsOf(m.name)) });
  }
  document.title = 'INFO-EXPORT ' + JSON.stringify({ foot_own: FX_INFO_FOOT_OWN, foot_like: FX_LIKE_FOOT, foot_players: FX_ARTIST_FOOT, models: out });
}, 800);
</script>"""


def find_editor():
    for folder in ROOT.parents:
        if (folder / 'nano-cortex-editor' / 'index.html').is_file():
            return folder / 'nano-cortex-editor'
    return None


def read_editor():
    """The editor's notes for every model, as JSON text; None when the editor or Chrome is missing."""
    editor = find_editor()
    chrome = os.environ.get('CHROME', '/Applications/Google Chrome.app/Contents/MacOS/Google Chrome')
    if not editor or not os.path.exists(chrome):
        return None
    page = (editor / 'index.html').read_text()
    if 'const FX_INFO_MORE = {' not in page:
        return None
    with tempfile.TemporaryDirectory() as folder:
        file = pathlib.Path(folder) / 'export.html'
        file.write_text(page.replace('</body>', EXPORT_SCRIPT + '</body>', 1))
        dom = subprocess.run([chrome, '--headless=new', '--disable-gpu', '--virtual-time-budget=8000', '--dump-dom', 'file://' + str(file)],
                             capture_output=True, text=True).stdout
    found = re.search(r'<title>INFO-EXPORT (.*?)</title>', dom, re.S)
    if not found:
        return None
    return json.dumps(json.loads(html.unescape(found.group(1))), indent=1)


def c_str(s):
    return '"' + s.replace('\\', '\\\\').replace('"', '\\"').replace('\n', ' ') + '"'


def main():
    text = read_editor()
    if text:
        INFO_JSON.write_text(text + '\n')
        print('tools/editor_info.json from the editor: %d models' % len(json.loads(text)['models']))
    elif not INFO_JSON.is_file():
        sys.exit('No editor (with Chrome) next to this project and no tools/editor_info.json')
    info = json.loads(INFO_JSON.read_text())

    models = {m[0]: m for m in MODELS['models']}
    tables, entries = [], []
    sounds_total = 0
    for entry in info['models']:
        mtype, name = entry['type'], entry['name']
        if mtype not in models:
            sys.exit('%s (%d) is not in the model table' % (name, mtype))
        params = models[mtype][4]

        def sounds(kind, ident):
            rows = []
            for s, sound in enumerate(entry[kind]):
                if len(sound['values']) != sound['asked']:
                    sys.exit('%s / %s: a control or a switch position the editor does not know' % (name, sound['name']))
                values = []
                for index, control, value in sound['values']:
                    if index >= len(params) or params[index][0].lower() != control.lower():
                        sys.exit('%s / %s: control %d is "%s" in the editor, not in the model table' % (name, sound['name'], index, control))
                    if not 0 <= value <= 1:
                        sys.exit('%s / %s: %s = %r' % (name, sound['name'], control, value))
                    values.append((index, value))
                values.sort()
                tables.append('static const nano_focus_value_t %s_%d[] = { %s };' % (ident, s, ', '.join('{ %d, %.4ff }' % v for v in values)))
                rows.append('    { %s, %s, %s, %d, %s_%d },' % (c_str(sound['name']), c_str(sound['text']), c_str(sound['why']), len(values), ident, s))
            if rows:
                tables.append('static const nano_info_sound_t %s[] = {\n%s\n};' % (ident, '\n'.join(rows)))
            return len(rows)

        starts, players = sounds('starts', 'i%d_s' % mtype), sounds('players', 'i%d_p' % mtype)
        if starts + players > MAX_SOUNDS:
            sys.exit('%s: %d starting points and players\' sounds, the page takes %d' % (name, starts + players, MAX_SOUNDS))
        sounds_total += starts + players
        tips = entry['tips']
        if tips:
            tables.append('static const char *const i%d_t[] = { %s };' % (mtype, ', '.join(c_str(t) for t in tips)))
        original = entry['basedOn'] or entry['like']
        entries.append('    { %d, %s, %s, %s, %s, %s, %s, %s, %d, %d, %d, %s, %s, %s },   // %s' % (
            mtype, c_str(entry['cat']), c_str(original), 'true' if entry['basedOn'] else 'false',
            'true' if entry['basedOn'] or entry['photo'] else 'false',
            c_str(entry['made']), c_str(entry['text']), c_str(entry['heard']), len(tips), starts, players,
            'i%d_t' % mtype if tips else 'NULL', 'i%d_s' % mtype if starts else 'NULL', 'i%d_p' % mtype if players else 'NULL', name))

    head = '// Generated by tools/gen_info.py from tools/editor_info.json - do not edit.'
    (ROOT / 'main' / 'fx_info.h').write_text('\n'.join([
        head,
        '// What the FX editor\'s info page tells about a model - the notes of the Nano Cortex Editor\'s large',
        '// effect view. A value is the Nano\'s 0-1 of the parameter (a switch: its position).',
        '#pragma once', '#include <stdbool.h>', '#include <stdint.h>', '', '#include "fx_focus.h"', '',
        '// A starting point or a player\'s sound: values to set with one tap.',
        'typedef struct {',
        '    const char *name;',
        '    const char *values_text;   // the values as the editor writes them ("Gain 2.5 \u00b7 Tone 4.5")',
        '    const char *why;           // a line about it, "" = none',
        '    uint8_t count;',
        '    const nano_focus_value_t *values;',
        '} nano_info_sound_t;', '',
        'typedef struct {',
        '    uint32_t type;',
        '    const char *category;',
        '    const char *original;      // the device, "" = none named',
        '    bool based;                // true: the model is based on it; false: it is in the style of it (for orientation)',
        '    bool picture;              // the pedal\'s picture belongs beside it (never a stand-in beside "In the style of")',
        '    const char *made, *text, *heard;   // "" = nothing to say',
        '    uint8_t tip_count, start_count, player_count;',
        '    const char *const *tips;',
        '    const nano_info_sound_t *starts, *players;',
        '} nano_info_t;', '',
        '// The notes of a model, NULL if there are none.',
        'const nano_info_t *nano_fx_info(uint32_t type);', '',
        '// The small print: below a model with an original, one "in the style of", and below players\' sounds.',
        'extern const char NANO_INFO_FOOT_BASED[], NANO_INFO_FOOT_LIKE[], NANO_INFO_FOOT_PLAYERS[];', '']))
    (ROOT / 'main' / 'fx_info.c').write_text('\n'.join([
        head, '#include "fx_info.h"', '#include <stddef.h>', ''] + tables + [
        '', 'static const nano_info_t INFO[] = {'] + entries + ['};', '',
        'const char NANO_INFO_FOOT_BASED[] = %s;' % c_str(info['foot_own']),
        'const char NANO_INFO_FOOT_LIKE[] = %s;' % c_str(info['foot_like']),
        'const char NANO_INFO_FOOT_PLAYERS[] = %s;' % c_str(info['foot_players']), '',
        'const nano_info_t *nano_fx_info(uint32_t type)', '{',
        '    for (size_t i = 0; i < sizeof(INFO) / sizeof(INFO[0]); i++) {',
        '        if (INFO[i].type == type) return &INFO[i];', '    }', '    return NULL;', '}', '']))
    print('main/fx_info.c: %d models, %d starting points and players\' sounds' % (len(entries), sounds_total))


if __name__ == '__main__':
    main()
