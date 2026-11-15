#!/usr/bin/env python3
"""Generates main/fx_focus.h/.c: the four sounds in the corners of the FX editor's pad page, per FX model.

They are the Nano Cortex Editor's (its focus view, FOCUS_CORNERS in index.html), so the controller and the editor
offer the same sounds:

- With the editor's folder next to this project (and Google Chrome, or CHROME=<path>) the editor's page is run and
  asked for its table, every value already as the Nano's 0-1 of its control - the editor's own conversion, with its
  units and skews. It is kept as tools/editor_focus.json, so the firmware builds without the editor.
- From tools/editor_focus.json the C table is written, checked against tools/editor_models.json: a model or a
  control the model table does not have (or has under another number) stops the run.

Run from the project folder:  python3 tools/gen_focus.py
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
FOCUS_JSON = ROOT / 'tools' / 'editor_focus.json'
CORNERS = ['top left', 'top right', 'bottom left', 'bottom right']


def find_editor():
    for folder in ROOT.parents:
        if (folder / 'nano-cortex-editor' / 'index.html').is_file():
            return folder / 'nano-cortex-editor'
    return None


def read_editor():
    """The editor's FOCUS_CORNERS for every model that has them, each value already as the Nano's 0-1 of its control
    (the editor's own conversion, so its units and skews apply): the page runs in headless Chrome with a small
    script behind it. JSON text, or None when the editor or Chrome is missing."""
    editor = find_editor()
    chrome = os.environ.get('CHROME', '/Applications/Google Chrome.app/Contents/MacOS/Google Chrome')
    if not editor or not os.path.exists(chrome):
        return None
    page = (editor / 'index.html').read_text()
    if 'const FOCUS_CORNERS = {' not in page:
        return None
    script = """<script>
setTimeout(() => {
  const out = [], seen = {};
  for (const key of FX_SLOT_KEYS) for (const m of state.blocks[key].models) {
    const type = modelType(m), corners = FOCUS_CORNERS[focusModelKey(m.name)];
    if (!type || !corners || seen[type]) continue;
    seen[type] = true;
    const block = { params: structuredClone(m.params) };
    out.push({ type, name: m.name, corners: corners.map(c => ({ name: c.name, asked: Object.keys(c.values).length,
      values: fxStartParams(block, c).map(({ param, value }) =>
        [param.index, param.name, Number(paramValueToNormalized(Object.assign({}, param, { value })).toFixed(4))]) })) });
  }
  document.title = 'FOCUS-EXPORT ' + JSON.stringify(out);
}, 800);
</script>"""
    with tempfile.TemporaryDirectory() as folder:
        file = pathlib.Path(folder) / 'export.html'
        file.write_text(page.replace('</body>', script + '</body>', 1))
        dom = subprocess.run([chrome, '--headless=new', '--disable-gpu', '--virtual-time-budget=8000', '--dump-dom', 'file://' + str(file)],
                             capture_output=True, text=True).stdout
    found = re.search(r'<title>FOCUS-EXPORT (.*?)</title>', dom, re.S)
    if not found:
        return None
    return json.dumps(json.loads(html.unescape(found.group(1))), indent=1)


def c_str(s):
    return '"' + s.replace('\\', '\\\\').replace('"', '\\"') + '"'


def main():
    text = read_editor()
    if text:
        FOCUS_JSON.write_text(text + '\n')
        print('tools/editor_focus.json from the editor: %d models' % len(json.loads(text)))
    elif not FOCUS_JSON.is_file():
        sys.exit('No editor next to this project and no tools/editor_focus.json')
    focus = json.loads(FOCUS_JSON.read_text())

    models = {m[0]: m for m in MODELS['models']}
    tables, entries, notes = [], [], []
    for entry in focus:
        mtype, name = entry['type'], entry['name']
        if mtype not in models:
            sys.exit('%s (%d) is not in the model table' % (name, mtype))
        params = models[mtype][4]
        if len(entry['corners']) != 4:
            sys.exit('%s: %d corners' % (name, len(entry['corners'])))
        rows = []
        for c, corner in enumerate(entry['corners']):
            if len(corner['values']) != corner['asked']:
                sys.exit('%s / %s: a control or a switch position the editor does not know' % (name, corner['name']))
            values = []
            for index, control, value in corner['values']:
                if index >= len(params) or params[index][0].lower() != control.lower():
                    sys.exit('%s / %s: control %d is "%s" in the editor, not in the model table' % (name, corner['name'], index, control))
                if not 0 <= value <= 1:
                    sys.exit('%s / %s: %s = %r' % (name, corner['name'], control, value))
                values.append((index, value))
            values.sort()
            ident = 'f%d_%d' % (mtype, c)
            tables.append('static const nano_focus_value_t %s[] = { %s };' % (ident, ', '.join('{ %d, %.4ff }' % v for v in values)))
            rows.append('{ %s, %d, %s }' % (c_str(corner['name']), len(values), ident))
        entries.append('    { %d, { %s } },   // %s' % (mtype, ', '.join(rows), name))

    head = '// Generated by tools/gen_focus.py from tools/editor_focus.json - do not edit.'
    (ROOT / 'main' / 'fx_focus.h').write_text('\n'.join([
        head,
        '// The four sounds in the corners of the FX editor\'s pad page, per FX model - the sounds of the Nano Cortex',
        '// Editor\'s focus view. A value is the Nano\'s 0-1 of the parameter (a switch: its position).',
        '#pragma once', '#include <stdint.h>', '',
        'typedef struct {', '    uint8_t param;', '    float value;', '} nano_focus_value_t;', '',
        'typedef struct {', '    const char *name;', '    uint8_t count;', '    const nano_focus_value_t *values;', '} nano_focus_corner_t;', '',
        'typedef struct {', '    uint32_t type;', '    nano_focus_corner_t corner[4];   // %s' % ', '.join(CORNERS), '} nano_focus_t;', '',
        '// The corners of a model, NULL if it has none (a single control, or a model this table does not know).',
        'const nano_focus_t *nano_focus(uint32_t type);', '']))
    (ROOT / 'main' / 'fx_focus.c').write_text('\n'.join([
        head, '#include "fx_focus.h"', '#include <stddef.h>', ''] + tables + [
        '', 'static const nano_focus_t FOCUS[] = {'] + entries + ['};', '',
        'const nano_focus_t *nano_focus(uint32_t type)', '{',
        '    for (size_t i = 0; i < sizeof(FOCUS) / sizeof(FOCUS[0]); i++) {',
        '        if (FOCUS[i].type == type) return &FOCUS[i];', '    }', '    return NULL;', '}', '']))
    print('main/fx_focus.c: %d models, %d sounds' % (len(entries), 4 * len(entries)))


if __name__ == '__main__':
    main()
