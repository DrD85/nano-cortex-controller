// A stand-in for the board's web server (main/p4_net.c), to try the upload page and the Jam Player on the Mac without
// the board. Its folder plays the card's /sdcard/jam, as main/p4_jam.c keeps it (2026-11-27, albums): loose tracks
// <folder>/<Title>.wav, albums <folder>/<Album>/<Title>.wav, covers beside them (<name>.jpg), loudness.txt /
// levels.txt / sources.txt keyed by the relative file ("Album/Title.wav", loose "Title.wav").
// The API as the board's: GET /api/tracks ("albums":true, "more"; per track name, note, cover, source), PUT /api/tracks/<name>.wav
// (X-Lufs / X-Play / X-Peak / X-Trim / X-Source; into an album: a loose copy of the same title goes), PUT
// /api/covers/<name>.jpg, POST /api/move, POST /api/sources, POST /api/trims, POST /api/done; <name> "Album/Title"
// or "Title", each part URL-encoded. PUT /api/firmware is checked as the board does (writes nothing).
// Serves main/web/upload.html at / and the folder's files at /files/<file> (test files for the page to fetch).
// The looper's saved loops (main/p4_loops.c, 2026-11-28) from a folder of their own (the card's /sdcard/loops): GET
// /api/loops, GET /api/loops/<name> (in pieces, X-Size; a name with "(cut)" breaks off halfway, as a lost connection),
// DELETE /api/loops/<name>, POST /api/time (logged).
// node tools/mock-board/server.js [port] [folder] [loops folder]   (default 8765, $TMPDIR/mock-board, <folder>-loops)
// (no node on the Mac: ELECTRON_RUN_AS_NODE=1 "/Applications/Jam Player.app/Contents/MacOS/Jam Player" server.js)
'use strict';
const http = require('http');
const fs = require('fs');
const path = require('path');
const port = Number(process.argv[2] || 8765);
const dir = path.resolve(process.argv[3] || path.join(process.env.TMPDIR || '/tmp', 'mock-board'));
fs.mkdirSync(dir, { recursive: true });
const page = path.join(__dirname, '../../main/web/upload.html');
const loopsDir = path.resolve(process.argv[4] || dir + '-loops');
fs.mkdirSync(loopsDir, { recursive: true });
// As loop_wav_name_ok: "Loop <digits> ....wav" of letters, digits, spaces and , . - ( ) - no path, nothing hidden.
const loopNameOk = n => n.length >= 10 && n.length < 96 && /^Loop \d/.test(n) && /\.wav$/i.test(n) && !n.includes('..') && /^[A-Za-z0-9 ,.()-]+$/.test(n);
const ALBUM_MAX = 60, TITLE_MAX = 78, STAMP_MAX = 40, MAX_TRACKS = 256;   // as main/p4_audio.h

const bytes = s => Buffer.byteLength(s, 'utf8');
const file = (name, ext) => path.join(dir, name + '.' + ext);
const exists = f => fs.existsSync(f);

// As p4_jam_clean_name: the name FAT keeps (an album's trailing spaces / dots dropped; leading spaces stay, FAT keeps
// them in the long name), one folder level, no part empty or starting with '.', no character FAT refuses. null: not
// usable.
function cleanName(name) {
  const parts = name.split('/');
  if (parts.length > 2) return null;
  const clean = (s, folder, most) => {
    if (folder) s = s.replace(/[ .]+$/, '');
    if (!s || bytes(s) > most || s[0] === '.' || /[\\:*?"<>|\x00-\x1F\x7F]/.test(s)) return null;
    return s;
  };
  const title = clean(parts[parts.length - 1], false, TITLE_MAX);
  if (title === null) return null;
  if (parts.length === 1) return title;
  const album = clean(parts[0], true, ALBUM_MAX);
  return album === null ? null : album + '/' + title;
}

// "/api/tracks/Blues/Slow%20Blues.wav" -> "Blues/Slow Blues": each part decoded on its own (an encoded '/' refused).
function nameOf(rawPath, prefix, ext) {
  const parts = rawPath.slice(prefix.length).split('/');
  if (parts.length > 2) return null;
  let decoded;
  try { decoded = parts.map(p => decodeURIComponent(p)); } catch (err) { return null; }
  if (decoded.some(p => p.includes('/'))) return null;
  let name = decoded.join('/');
  if (name.toLowerCase().endsWith(ext)) name = name.slice(0, -ext.length);
  return cleanName(name);
}

// "<name>.wav<TAB><value>" lines: [name without .wav, value] (lines without a tab left out)
function lines(text) {
  return text.split('\n').map(l => l.replace(/\r.*$/, '')).filter(l => l.includes('\t')).map(l => {
    const tab = l.indexOf('\t');
    return [l.slice(0, tab).replace(/\.wav$/i, ''), l.slice(tab + 1)];
  });
}
const readKeep = what => { try { return fs.readFileSync(path.join(dir, what + '.txt'), 'utf8'); } catch (err) { return ''; } };
const appendKeep = (what, line) => fs.appendFileSync(path.join(dir, what + '.txt'), line + '\n');
function writeKeep(what, text) {   // a new file, then renamed (as the board)
  const tmp = path.join(dir, what + '.tmp');
  fs.writeFileSync(tmp, text);
  fs.renameSync(tmp, path.join(dir, what + '.txt'));
}

// The card's tracks as the board lists them: the loose first, then album by album, each by title (case not counted).
// More than MAX_TRACKS: an album that does not fit whole is left out whole (albums go in by name), loose tracks past
// the end one by one; list.leftOut counts them (GET /api/tracks "more").
function scan() {
  const list = [];
  let leftOut = 0;
  const cmp = (a, b) => a.toLowerCase() < b.toLowerCase() ? -1 : a.toLowerCase() > b.toLowerCase() ? 1 : 0;
  const track = (album, f) => {
    if (f[0] === '.' || !/\.wav$/i.test(f) || f.length < 5) return null;
    const title = f.slice(0, -4), name = album ? album + '/' + title : title;
    if (bytes(name) >= 144) return null;
    return { name, album, title, size: fs.statSync(path.join(dir, name + f.slice(-4))).size };
  };
  const albums = [];
  for (const e of fs.readdirSync(dir, { withFileTypes: true })) {
    if (e.name[0] === '.') continue;
    if (!e.isDirectory()) {
      const t = e.isFile() && track('', e.name);
      if (t && list.length < MAX_TRACKS) list.push(t); else if (t) leftOut++;
      continue;
    }
    if (bytes(e.name) <= ALBUM_MAX) albums.push(e.name);
  }
  for (const album of albums.sort(cmp)) {
    const tracks = fs.readdirSync(path.join(dir, album), { withFileTypes: true }).filter(f => f.isFile()).map(f => track(album, f.name)).filter(Boolean);
    if (list.length + tracks.length <= MAX_TRACKS) list.push(...tracks);
    else { leftOut += tracks.length; console.log(`album ${album} (${tracks.length} tracks) left out: the list holds ${MAX_TRACKS}`); }
  }
  list.sort((a, b) => (!a.album !== !b.album ? (a.album ? 1 : -1) : 0) || cmp(a.album, b.album) || cmp(a.title, b.title) || cmp(a.name, b.name));
  const loud = new Map(), source = new Map();
  for (const [name, v] of lines(readKeep('loudness'))) loud.set(name, v.split('\t').map(Number));
  for (const [name, v] of lines(readKeep('sources'))) source.set(name, v);
  for (const t of list) {
    const l = loud.get(t.name);
    t.lufs = l && l[0] === t.size ? l[1] : NaN;
    t.source = source.get(t.name) || '';
  }
  list.leftOut = leftOut;
  return list;
}

// One of the text files with the moved tracks' keys renamed; a target's old line dropped (as main/p4_jam.c move_keys).
function moveKeys(what, moves) {
  const text = readKeep(what);
  if (!text) return;
  const out = [];
  for (const line of text.split('\n')) {
    const tab = line.indexOf('\t');
    if (tab < 0) continue;
    const key = line.slice(0, tab).replace(/\.wav$/i, '');
    const m = moves.find(m => m.from === key || m.to === key);
    if (!m) out.push(line);
    else if (m.from === key) out.push(m.to + '.wav' + line.slice(tab));
  }
  writeKeep(what, out.map(l => l + '\n').join(''));
}

function body(req, done) {
  const chunks = [];
  req.on('data', c => chunks.push(c));
  req.on('end', () => done(Buffer.concat(chunks)));
}
const send = (res, status, obj) => { res.writeHead(status, { 'Content-Type': 'application/json' }); res.end(JSON.stringify(obj)); };
const stampOk = s => typeof s === 'string' && s.length > 0 && s.length <= STAMP_MAX && /^[\x21-\x7E]+$/.test(s);

http.createServer((req, res) => {
  const raw = req.url.split('?')[0];
  let url;
  try { url = decodeURIComponent(raw); } catch (err) { url = raw; }
  if (req.method === 'GET' && url === '/') {
    res.writeHead(200, { 'Content-Type': 'text/html; charset=utf-8' });
    return res.end(fs.readFileSync(page));
  }
  if (req.method === 'GET' && url.startsWith('/files/')) {   // test files for the page to fetch
    const f = path.join(dir, path.basename(url));
    if (!fs.existsSync(f)) { res.writeHead(404); return res.end(); }
    return res.end(fs.readFileSync(f));
  }
  if (req.method === 'GET' && url === '/api/tracks') {
    const tracks = scan();
    const list = tracks.map(t => ({
      name: t.name,
      note: Number.isFinite(t.lufs) ? (t.lufs < 0 ? '−' : '') + Math.abs(t.lufs).toFixed(1) + ' LUFS' : 'MEASURING',
      cover: exists(file(t.name, 'jpg')),
      source: t.source,
      size: t.size,
    }));
    return send(res, 200, { firmware: '0.1.0', albums: true, card: true, measuring: 0, more: tracks.leftOut, tracks: list });
  }
  if (req.method === 'POST' && url === '/api/done') { console.log('done'); return send(res, 200, { ok: true }); }
  if (req.method === 'GET' && url === '/api/loops') {
    const loops = fs.readdirSync(loopsDir).filter(loopNameOk).map(name => ({ name, size: fs.statSync(path.join(loopsDir, name)).size }));
    return send(res, 200, { card: true, loops });
  }
  if (raw.startsWith('/api/loops/') && (req.method === 'GET' || req.method === 'DELETE')) {
    let name = null;
    try { name = decodeURIComponent(raw.slice('/api/loops/'.length)); } catch (err) { /* refused below */ }
    if (!name || !loopNameOk(name)) return send(res, 400, { ok: false, error: "not a loop's name" });
    const f = path.join(loopsDir, name);
    if (!exists(f)) return send(res, 404, { ok: false, error: 'no such loop' });
    if (req.method === 'DELETE') {
      fs.rmSync(f);
      console.log('deleted loop ' + name);
      return send(res, 200, { ok: true });
    }
    const size = fs.statSync(f).size;
    res.writeHead(200, { 'Content-Type': 'audio/wav', 'X-Size': String(size) });   // (no length: in pieces, as the board)
    if (name.includes('(cut)')) {   // a connection lost halfway
      res.write(fs.readFileSync(f).subarray(0, size >> 1));
      return setTimeout(() => res.destroy(), 50);
    }
    console.log('sending loop ' + name);
    return fs.createReadStream(f).pipe(res);
  }
  if (req.method === 'POST' && url === '/api/time') {
    return body(req, b => {
      const [t, minutes] = b.toString('utf8').trim().split(/\s+/).map(Number);
      if (!Number.isFinite(t) || !Number.isFinite(minutes)) return send(res, 400, { ok: false, error: 'not a time' });
      console.log(`time ${new Date(t * 1000).toISOString()} (UTC ${minutes >= 0 ? '+' : ''}${minutes} min)`);
      send(res, 200, { ok: true });
    });
  }
  if (req.method === 'POST' && url === '/api/trims') {   // merged into levels.txt as the board does (0 removes)
    return body(req, b => {
      const text = b.toString('utf8');
      console.log('trims ' + JSON.stringify(text));
      const levels = new Map(lines(readKeep('levels')));
      for (const [n, v] of lines(text)) { const name = cleanName(n); if (name) levels.set(name, v); }
      writeKeep('levels', [...levels].filter(([, v]) => Math.abs(parseFloat(v)) >= 0.05)
        .map(([n, v]) => `${n}.wav\t${parseFloat(v).toFixed(1)}\n`).join(''));
      send(res, 200, { ok: true });
    });
  }
  if (req.method === 'POST' && url === '/api/sources') {   // stamps for tracks on the card already
    return body(req, b => {
      let n = 0;
      for (const [raw, stamp] of lines(b.toString('utf8'))) {
        const name = cleanName(raw);
        if (name && stampOk(stamp)) { appendKeep('sources', `${name}.wav\t${stamp}`); n++; }
      }
      console.log(JSON.stringify({ sources: n }));
      send(res, 200, { ok: true });
    });
  }
  if (req.method === 'POST' && url === '/api/move') {   // loose tracks into their albums, with cover and keys
    return body(req, b => {
      const moves = [];
      for (const [rawFrom, rawTo] of lines(b.toString('utf8'))) {
        // "from" names a file the board listed: taken as it is spelt (cleaning it could miss the file), only checked
        const from = cleanName(rawFrom) === rawFrom ? rawFrom : null, to = cleanName(rawTo.replace(/\.wav$/i, ''));
        if (!from || !to || from.toLowerCase() === to.toLowerCase()) continue;
        if (moves.some(m => m.from.toLowerCase() === from.toLowerCase() || m.to.toLowerCase() === to.toLowerCase())) continue;
        if (!exists(file(from, 'wav')) || exists(file(to, 'wav'))) {   // (FAT's rename replaces nothing)
          console.log(`move ${from} -> ${to}: not moved (${exists(file(to, 'wav')) ? 'the target is there already' : 'no such track'})`);
          continue;
        }
        if (to.includes('/')) fs.mkdirSync(path.join(dir, to.split('/')[0]), { recursive: true });
        fs.renameSync(file(from, 'wav'), file(to, 'wav'));
        for (const ext of ['jpg', 'jpeg']) if (exists(file(from, ext))) fs.renameSync(file(from, ext), file(to, ext));
        moves.push({ from, to });
        console.log(`moved ${from} -> ${to}`);
      }
      if (moves.length) for (const what of ['levels', 'loudness', 'sources']) moveKeys(what, moves);
      send(res, 200, { ok: true, moved: moves.length });
    });
  }
  if (req.method === 'PUT' && url === '/api/firmware') {   // checks as main/p4_ota.c does, writes nothing
    return body(req, b => {
      const str = (o, n) => b.slice(o, o + n).toString('latin1').replace(/\0.*$/s, '');
      let error = null;
      if (b.length < 288 || b[0] !== 0xE9) error = 'not a firmware file (use build/nano_controller_p4.bin)';
      else if (b.readUInt16LE(12) !== 18) error = 'a firmware for another chip (the small board?)';
      else if (b.readUInt32LE(32) !== 0xABCD5432) error = 'a firmware file without its description';
      else if (str(80, 32) !== 'nano_controller_p4') error = 'a firmware of another project (' + str(80, 32) + '), not nano_controller_p4';
      console.log('firmware ' + b.length + ' bytes: ' + (error || 'ok, version ' + str(48, 32)));
      if (error) return send(res, 400, { ok: false, error });
      send(res, 200, { ok: true, version: str(48, 32), restart: true });
    });
  }
  if (req.method === 'PUT' && raw.startsWith('/api/covers/')) {   // as main/p4_net.c cover_upload: a JPEG, 2 MB at most
    const name = nameOf(raw, '/api/covers/', '.jpg');
    return body(req, b => {
      if (!name) return send(res, 400, { ok: false, error: 'unusable file name' });
      if (b.length < 4 || b.length > 2 * 1024 * 1024 || b[0] !== 0xFF || b[1] !== 0xD8) return send(res, 400, { ok: false, error: 'not a JPEG' });
      const progressive = b.includes(Buffer.from([0xFF, 0xC2]));
      if (name.includes('/')) fs.mkdirSync(path.join(dir, name.split('/')[0]), { recursive: true });
      fs.writeFileSync(file(name, 'jpg'), b);
      console.log(JSON.stringify({ cover: name, bytes: b.length, progressive }));
      send(res, 200, { ok: true });
    });
  }
  if (req.method === 'PUT' && raw.startsWith('/api/tracks/')) {
    const name = nameOf(raw, '/api/tracks/', '.wav');
    return body(req, b => {
      if (!name) return send(res, 400, { ok: false, error: 'unusable file name' });
      if (b.slice(0, 4).toString() !== 'RIFF' || b.slice(8, 12).toString() !== 'WAVE') return send(res, 400, { ok: false, error: 'not a WAV file' });
      if (name.includes('/')) fs.mkdirSync(path.join(dir, name.split('/')[0]), { recursive: true });
      const tmp = path.join(dir, 'upload.tmp');
      fs.writeFileSync(tmp, b);
      fs.renameSync(tmp, file(name, 'wav'));   // (a new version replaces the old one)
      let removed = false;
      if (name.includes('/')) {   // into an album: the loose copy of the same title from before albums goes
        const loose = name.slice(name.indexOf('/') + 1);
        if (exists(file(loose, 'wav'))) {
          fs.rmSync(file(loose, 'wav'));
          for (const ext of ['jpg', 'jpeg']) fs.rmSync(file(loose, ext), { force: true });
          removed = true;
        }
      }
      const h = k => parseFloat(req.headers[k]);
      const [lufs, play, peak, trim] = [h('x-lufs'), h('x-play'), h('x-peak'), h('x-trim')];
      if ([lufs, play, peak].every(Number.isFinite) && peak > 0)
        appendKeep('loudness', `${name}.wav\t${b.length}\t${lufs.toFixed(2)}\t${play.toFixed(2)}\t${peak.toFixed(5)}`);
      else appendKeep('loudness', `${name}.wav\t0\t0.00\t0.00\t0.00000`);   // "not measured": the old one must not stay
      if (Number.isFinite(trim) && Math.abs(trim) >= 0.05) appendKeep('levels', `${name}.wav\t${trim.toFixed(1)}`);
      const source = req.headers['x-source'];
      appendKeep('sources', `${name}.wav\t${stampOk(source) ? source : ''}`);   // (none clears the one before)
      console.log(JSON.stringify({ name, bytes: b.length, lufs, play, peak, trim, source: source || null, looseRemoved: removed }));
      send(res, 200, { ok: true });
    });
  }
  res.writeHead(404);
  res.end();
}).listen(port, () => console.log(`mock board on http://localhost:${port} (its card's jam folder: ${dir}, loops: ${loopsDir})`));
