"""Effect icons for the tiles and the FX editor (own drawings from the Nano Cortex Editor, tools/icons.py).

24x24 line icons as SVG markup; tools/gen_tables.py renders them into LVGL A8 masks.
"""

ICONS = {
    'overdrive': '<path d="M2 12h3l2-6h5l2 6h0l2 6h5l2-6"/>',
    'bass-overdrive': '<path d="M2 12c2 0 3-7 6-7h4c3 0 4 14 7 14h3"/><path d="M5 21h14"/>',
    'fuzz': '<path d="M2 17h4V7h6v10h6V7h4"/>',
    'compressor': '<path d="M12 2v6m-3-3 3 3 3-3"/><path d="M12 22v-6m-3 3 3-3 3 3"/><path d="M3 12h18"/>',
    'equalizer': '<path d="M6 3v18M12 3v18M18 3v18"/><rect x="4" y="13" width="4" height="3" rx="1"/><rect x="10" y="6" width="4" height="3" rx="1"/><rect x="16" y="10" width="4" height="3" rx="1"/>',
    'filter': '<path d="M2 8h9c3 0 4 2 5 5s2 7 6 7"/><path d="M2 20h20" stroke-dasharray="1 3"/>',
    'wah': '<path d="M4 19h16"/><path d="M5 15l14-7"/><path d="M6 19l-1-4M18 19l1-11"/>',
    'utility': '<path d="M3 7h10M17 7h4M3 17h4M11 17h10"/><circle cx="15" cy="7" r="2"/><circle cx="9" cy="17" r="2"/>',
    'gate': '<path d="M2 12c1-5 2-5 3 0s2 5 3 0"/><path d="M8 12h8"/><path d="M16 12c1-5 2-5 3 0s2 5 3 0"/><path d="M11 6v12M13 6v12"/>',
    'doubler': '<path d="M2 10c3-6 6-6 9 0s6 6 9 0"/><path d="M4 15c3-6 6-6 9 0s6 6 9 0"/>',
    'pitch': '<path d="M9 18V5l10-2v13"/><circle cx="6" cy="18" r="3"/><circle cx="16" cy="16" r="3"/>',
    'modulation': '<path d="M2 12c2.5-7 5-7 7.5 0s5 7 7.5 0 3.5-5 5-3"/>',
    'delay': '<path d="M4 6v12M10 9v6M15 11v2M19.5 11.5v1"/>',
    'reverb': '<circle cx="5" cy="12" r="1.5"/><path d="M9 8a6 6 0 0 1 0 8M13 5a10 10 0 0 1 0 14M17 2a14 14 0 0 1 0 20"/>',
    'capture': '<circle cx="12" cy="12" r="9"/><path d="M5 12c2-4 3-4 4.5 0s2.5 4 4.5 0 3-4 5 0"/>',
    'cab': '<rect x="4" y="3" width="16" height="18" rx="2"/><circle cx="12" cy="13" r="4.5"/><circle cx="12" cy="13" r="1"/>',
    'tuner': '<path d="M8 2v8a4 4 0 0 0 8 0V2"/><path d="M12 14v8"/>',
}
