// Compares the board's loudness (test_loudness.c wrote board.json and the signals) with the Jam Player's own
// measuring code, taken unchanged from jam-player/index.html (the loudness worker). Exit code 1 on a difference.
// node loudness_check.js <folder> <path to jam-player/index.html>
'use strict';
const fs = require('fs');
const [dir, page] = process.argv.slice(2);
const html = fs.readFileSync(page, 'utf8');
const worker = html.match(/<script id="loudnessWorker"[^>]*>([\s\S]*?)<\/script>/)[1];
const trackLoudness = new Function(worker.replace(/onmessage[\s\S]*$/, '') + '\nreturn trackLoudness;')();

let failed = 0;
for (const b of JSON.parse(fs.readFileSync(dir + '/board.json', 'utf8'))) {
  const raw = fs.readFileSync(`${dir}/${b.name}.raw`);
  const pcm = new Int16Array(raw.buffer, raw.byteOffset, raw.length / 2);
  const n = pcm.length / 2;
  const l = new Float32Array(n), r = new Float32Array(n);
  for (let i = 0; i < n; i++) { l[i] = pcm[2 * i] / 32768; r[i] = pcm[2 * i + 1] / 32768; }
  const channels = b.mono ? [l] : [l, r];   // the browser decodes a mono file as one channel
  const js = trackLoudness({ rate: b.rate, channels, bassHz: 200, bassUsualDb: 1.4, bassMostDb: 3 });
  const d = Math.max(Math.abs(js.lufs - b.lufs), Math.abs(js.play - b.play), Math.abs(20 * Math.log10(js.peak / b.peak)));
  const ok = d < 0.01;
  if (!ok) failed++;
  console.log(`  ${ok ? 'ok  ' : 'FAIL'}  ${b.name.padEnd(8)} player lufs ${js.lufs.toFixed(3)} play ${js.play.toFixed(3)} | board ${b.lufs.toFixed(3)} ${b.play.toFixed(3)} (largest difference ${d.toFixed(4)} dB)`);
}
process.exit(failed ? 1 : 0);
