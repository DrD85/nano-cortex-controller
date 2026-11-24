# Lists the places in a ui.c where a function that other tasks call touches LVGL while it does not hold the screen lock:
# before its first lvgl_port_lock(), after its last lvgl_port_unlock(), or with no lock at all.
import re, sys
s = open(sys.argv[1]).read()
funcs = {}
pat = re.compile(r'^(static\s+)?(?:inline\s+)?([A-Za-z_][\w \*]*?)\b(\w+)\s*\(([^;{}]*)\)\s*\{', re.M)
for m in pat.finditer(s):
    start = m.end(); depth = 1; i = start
    while depth and i < len(s):
        c = s[i]
        depth += c == '{'; depth -= c == '}'
        i += 1
    funcs[m.group(3)] = (bool(m.group(1)), s[start:i - 1], s.count('\n', 0, m.start()) + 1)
call = re.compile(r'\b(\w+)\s*\(')
def touches(body, seen=None):          # does this code reach LVGL (directly or through static helpers)?
    seen = seen or set()
    for name in call.findall(body):
        if name.startswith('lv_') and name not in ('lv_color_hex', 'lv_tick_elaps', 'lv_tick_get', 'lv_color_make', 'lv_pct'):
            return name
        if name in funcs and funcs[name][0] and name not in seen:
            seen.add(name)
            t = touches(funcs[name][1], seen)
            if t: return name + ' -> ' + t
    return None
n = 0
for name, (static, body, line) in sorted(funcs.items(), key=lambda kv: kv[1][2]):
    if static: continue
    first = body.find('lvgl_port_lock'); last = body.rfind('lvgl_port_unlock')
    if first < 0:
        t = touches(body)
        if t: n += 1; print('%5d %-32s no lock at all: %s' % (line, name, t))
        continue
    t = touches(body[:first])
    if t: n += 1; print('%5d %-32s before the lock: %s' % (line, name, t))
    if last >= 0:
        t = touches(body[last + len('lvgl_port_unlock()'):])
        if t: n += 1; print('%5d %-32s after the unlock: %s' % (line, name, t))
    if body.count('lvgl_port_lock') != body.count('lvgl_port_unlock'):
        print('%5d %-32s %d locks, %d unlocks' % (line, name, body.count('lvgl_port_lock'), body.count('lvgl_port_unlock')))
print(n, 'findings in', sys.argv[1])
