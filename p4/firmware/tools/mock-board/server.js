// A stand-in for the board's web server (main/p4_net.c), to try the upload page on the Mac without the board:
// serves main/web/upload.html, takes PUT /api/tracks/<name>.wav into a folder, lists them, logs /api/trims, answers /api/done,
// checks PUT /api/firmware as the board does (writes nothing).
// node tools/mock-board/server.js [port] [folder]   (default 8765, $TMPDIR/mock-board)
'use strict';
const http = require('http');
const fs = require('fs');
const path = require('path');
const port = Number(process.argv[2] || 8765);
const dir = process.argv[3] || path.join(process.env.TMPDIR || '/tmp', 'mock-board');
fs.mkdirSync(dir, { recursive: true });
const page = path.join(__dirname, '../../main/web/upload.html');
const tracks = new Map();   // name -> { size, lufs, play, peak }

http.createServer((req, res) => {
  const url = decodeURIComponent(req.url.split('?')[0]);
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
    res.writeHead(200, { 'Content-Type': 'application/json' });
    const list = [...tracks].sort().map(([name, t]) => ({ name, note: isFinite(t.lufs) ? `${t.lufs.toFixed(1)} LUFS` : 'MEASURING' }));
    return res.end(JSON.stringify({ firmware: '0.1.0', card: true, measuring: 0, tracks: list }));
  }
  if (req.method === 'POST' && url === '/api/done') { console.log('done'); return res.end('{"ok":true}'); }
  if (req.method === 'POST' && url === '/api/trims') {
    let body = '';
    req.on('data', c => { body += c; });
    req.on('end', () => { console.log('trims ' + JSON.stringify(body)); res.end('{"ok":true}'); });
    return;
  }
  if (req.method === 'PUT' && url === '/api/firmware') {   // checks as main/p4_ota.c does, writes nothing
    const chunks = [];
    req.on('data', c => chunks.push(c));
    req.on('end', () => {
      const b = Buffer.concat(chunks);
      const str = (o, n) => b.slice(o, o + n).toString('latin1').replace(/\0.*$/s, '');
      let error = null;
      if (b.length < 288 || b[0] !== 0xE9) error = 'not a firmware file (use build/nano_controller_p4.bin)';
      else if (b.readUInt16LE(12) !== 18) error = 'a firmware for another chip (the small board?)';
      else if (b.readUInt32LE(32) !== 0xABCD5432) error = 'a firmware file without its description';
      else if (str(80, 32) !== 'nano_controller_p4') error = 'a firmware of another project (' + str(80, 32) + '), not nano_controller_p4';
      console.log('firmware ' + b.length + ' bytes: ' + (error || 'ok, version ' + str(48, 32)));
      if (error) { res.writeHead(400); return res.end(JSON.stringify({ ok: false, error })); }
      res.end(JSON.stringify({ ok: true, version: str(48, 32), restart: true }));
    });
    return;
  }
  if (req.method === 'PUT' && url.startsWith('/api/tracks/')) {
    const name = url.slice('/api/tracks/'.length).replace(/\.wav$/i, '');
    const chunks = [];
    req.on('data', c => chunks.push(c));
    req.on('end', () => {
      const body = Buffer.concat(chunks);
      const ok = body.slice(0, 4).toString() === 'RIFF' && body.slice(8, 12).toString() === 'WAVE';
      if (!ok) { res.writeHead(400); return res.end('{"ok":false,"error":"not a WAV file"}'); }
      fs.writeFileSync(path.join(dir, 'received-' + name + '.wav'), body);
      const h = k => parseFloat(req.headers[k]);
      tracks.set(name, { size: body.length, lufs: h('x-lufs'), play: h('x-play'), peak: h('x-peak') });
      console.log(JSON.stringify({ name, bytes: body.length, lufs: h('x-lufs'), play: h('x-play'), peak: h('x-peak') }));
      res.end('{"ok":true}');
    });
    return;
  }
  res.writeHead(404);
  res.end();
}).listen(port, () => console.log(`mock board on http://localhost:${port} (files in ${dir})`));
