// Nano Cortex Controller in the browser: the board's firmware (WebAssembly, nano-controller.js) with the screen on a
// canvas, the footswitches as buttons, Web Bluetooth to the Nano Cortex and Web MIDI for a MIDI controller.
'use strict';

const SERVICE_UUIDS = ['0000a002-0000-1000-8000-00805f9b34fb', '0000a003-0000-1000-8000-00805f9b34fb'];
const C304_UUID = '0000c304-0000-1000-8000-00805f9b34fb';
const C305_UUID = '0000c305-0000-1000-8000-00805f9b34fb';
const HOLD_MS = 600;                                // FOOTSWITCH_HOLD_MS of the board
const FS = { PRESS: 0, RELEASE: 1, HOLD: 2 };       // footswitch_event_t
const SCREEN_W = 800, SCREEN_H = 480;

const canvas = document.getElementById('screen');
const ctx = canvas.getContext('2d', { alpha: false });
const connectButton = document.getElementById('connect');
const statusText = document.getElementById('status');
const root = document.documentElement;
const miniConnect = document.getElementById('mini-connect');
const toast = document.getElementById('toast');
let toastTimer = null;

function setStatus(text, kind = '') {
  statusText.textContent = text;
  statusText.dataset.kind = kind;
  miniConnect.dataset.kind = kind;
  miniConnect.title = text;
  showToast(text, kind);
}

// A short message over the screen (screen-only mode has no status line).
function showToast(text, kind = '') {
  if (!root.classList.contains('screen-only')) return;
  toast.textContent = text;
  toast.dataset.kind = kind;
  toast.hidden = false;
  clearTimeout(toastTimer);
  toastTimer = setTimeout(() => { toast.hidden = true; }, kind === 'error' ? 6000 : 2500);
}

// ---- the firmware ----
// The module object: app.js offers the functions the firmware calls, the firmware adds its own (_web_...).
// Calls from the firmware are answered later (setTimeout / promises), never while it is still running.

const M = {
  ctx,
  nanoWrite(bytes) {
    const ch = link.c304;
    // Always answered later; writeValue for browsers without writeValueWithResponse (older Bluefy on the iPhone)
    Promise.resolve().then(() => {
      if (!ch) throw new Error('not connected');
      return ch.writeValueWithResponse ? ch.writeValueWithResponse(bytes) : ch.writeValue(bytes);
    }).then(
      () => M._web_nano_written(1),
      (e) => { console.warn('Write failed', e); M._web_nano_written(0); });
  },
  midiSearch(on) { setTimeout(() => midiSearch(on)); },
  midiConnect(index) { setTimeout(() => midiConnect(index)); },
  midiForget() { setTimeout(midiForget); },
  midiResume() { setTimeout(midiResume); },
  print: (text) => console.log(text),
  printErr: (text) => console.warn(text),
};

let started = false;
createNanoController(M).then(() => {
  started = true;
  setInterval(() => M._web_platform_run(), 100);   // timers also while the tab is in the background
  if (!navigator.bluetooth) {
    setStatus('Web Bluetooth is not available here - use Chrome or Edge (on iPhone: the Bluefy browser)', 'error');
  }
}).catch((e) => setStatus('Could not start: ' + e, 'error'));

function toFirmware(bytes, fn) {
  const p = M._malloc(bytes.length);
  M.HEAPU8.set(bytes, p);
  fn(p, bytes.length);
  M._free(p);
}

// ---- Web Bluetooth ----

const link = { device: null, c304: null, c305: null, ready: false, userClosed: false, retry: null };

function onNotify(event) {
  const v = event.target.value;
  toFirmware(new Uint8Array(v.buffer, v.byteOffset, v.byteLength), (p, n) => M._web_nano_packet(p, n));
}

// C304 (write) and C305 (notify) under service A002 or A003. Asked for by UUID: browsers report UUIDs in different
// forms (Bluefy on the iPhone e.g. in capitals or as "C304"), so the lists are not compared as text.
async function findCharacteristics(server) {
  const seen = [];
  for (const uuid of SERVICE_UUIDS) {
    let service;
    try { service = await server.getPrimaryService(uuid); } catch (e) { continue; }
    try {
      return { c304: await service.getCharacteristic(C304_UUID), c305: await service.getCharacteristic(C305_UUID) };
    } catch (e) {
      try { for (const ch of await service.getCharacteristics()) seen.push(ch.uuid); } catch (e2) {}
    }
  }
  return { seen };
}

async function openLink() {
  const device = link.device;
  setStatus('Connecting to ' + (device.name || 'the device') + ' ...', 'busy');
  const server = await device.gatt.connect();
  link.c304 = link.c305 = null;
  const found = await findCharacteristics(server);
  if (!found.c304) {
    link.userClosed = true;
    device.gatt.disconnect();
    throw new Error('"' + (device.name || 'This device') + '" is not a Nano Cortex (no C304/C305' +
      (found.seen.length ? ', found ' + found.seen.join(', ') : '') + ').');
  }
  link.c304 = found.c304;
  link.c305 = found.c305;
  link.c305.addEventListener('characteristicvaluechanged', onNotify);   // the same listener is added only once
  await link.c305.startNotifications();
  link.ready = true;
  keepAwake(true);
  connectButton.textContent = 'DISCONNECT';
  setStatus('Connected: ' + (device.name || 'Nano Cortex'), 'ok');
  M._web_nano_link(1);
}

function onDisconnected() {
  const was = link.ready;
  link.ready = false;
  link.c304 = link.c305 = null;
  if (was) M._web_nano_link(0);
  connectButton.textContent = 'CONNECT';
  if (link.userClosed || !link.device) {
    keepAwake(false);
    setStatus('Not connected');
    return;
  }
  setStatus('Connection lost - connecting again ...', 'busy');
  retryLater();
}

function retryLater() {
  clearTimeout(link.retry);
  link.retry = setTimeout(async () => {
    if (link.userClosed || link.ready || !link.device) return;
    try { await openLink(); } catch (e) { retryLater(); }
  }, 3000);
}

async function connect() {
  if (!navigator.bluetooth) {
    setStatus('Web Bluetooth is not available here - use Chrome or Edge (on iPhone: the Bluefy browser)', 'error');
    return;
  }
  let device;
  try {
    device = await navigator.bluetooth.requestDevice({ acceptAllDevices: true, optionalServices: SERVICE_UUIDS });
  } catch (e) {
    if (e.name !== 'NotFoundError') setStatus(e.message, 'error');   // NotFoundError = chooser closed
    return;
  }
  link.device = device;
  link.userClosed = false;
  device.addEventListener('gattserverdisconnected', onDisconnected);
  try {
    await openLink();
  } catch (e) {
    console.warn(e);
    setStatus(e.message || String(e), 'error');
    if (!link.userClosed) retryLater();
  }
}

connectButton.addEventListener('click', () => {
  if (!started) return;
  if (link.ready || link.retry && !link.userClosed && link.device) {
    link.userClosed = true;
    clearTimeout(link.retry);
    link.retry = null;
    if (link.device && link.device.gatt.connected) link.device.gatt.disconnect();
    else setStatus('Not connected');
    connectButton.textContent = 'CONNECT';
    return;
  }
  connect();
});

// The display stays on while the Nano is connected (where the browser allows it).
let wakeLock = null;

async function keepAwake(on) {
  if (!('wakeLock' in navigator)) return;
  try {
    if (on && !wakeLock && document.visibilityState === 'visible') {
      wakeLock = await navigator.wakeLock.request('screen');
      wakeLock.addEventListener('release', () => { wakeLock = null; });
    } else if (!on && wakeLock) {
      await wakeLock.release();
      wakeLock = null;
    }
  } catch (e) {
    console.warn('Screen stays not awake', e);
  }
}

document.addEventListener('visibilitychange', () => { if (link.ready) keepAwake(true); });   // released when hidden

// ---- screen only: the board's screen as large as possible ----
// Phones in landscape switch by themselves; SCREEN (or ?screen in the address) switches on a computer. A phone or
// tablet held upright (or with rotation lock on) gets the view turned by 90 degrees: hold it sideways.

const phoneLandscape = matchMedia('(pointer: coarse) and (orientation: landscape) and (max-height: 520px)');
const touchDevice = matchMedia('(pointer: coarse)');
let screenOn = false;
let screenMode = new URLSearchParams(location.search).has('screen') ? 'on' : 'auto';   // auto, on or off
const fullscreenOk = !!(document.fullscreenEnabled || document.webkitFullscreenEnabled);
document.getElementById('mini-full').hidden = !fullscreenOk;

function inFullscreen() {
  return !!(document.fullscreenElement || document.webkitFullscreenElement);
}

// The screen as large as possible without distortion: the visible area, its full height (also the strip of the
// home indicator), the control bar beside it on the right. Centred if there is room.
const device = document.querySelector('.device');
const safeProbe = document.getElementById('safe-probe');
const BAR_WIDTH = 54;   // control bar (44 px) and its gap

function layoutScreen() {
  const on = root.classList.contains('screen-only');
  device.classList.toggle('sized', on);
  if (!on) {
    for (const p of ['left', 'top', 'width', 'height']) canvas.style[p] = '';
    return;
  }
  const vv = window.visualViewport;
  const vw = vv ? vv.width : innerWidth, vh = vv ? vv.height : innerHeight;
  const ps = getComputedStyle(safeProbe), inset = (side) => parseFloat(ps['padding' + side]) || 0;
  const rotated = root.classList.contains('rotated');   // the portrait screen turned: its top edge is the view's left
  const W = rotated ? vh : vw, H = rotated ? vw : vh;
  const left = rotated ? inset('Top') : inset('Left'), right = W - (rotated ? inset('Bottom') : inset('Right')) - BAR_WIDTH;
  const width = Math.floor(Math.min(right - left, H * 5 / 3)), height = Math.floor(width * 3 / 5);
  const x = Math.max(left, Math.min((W - width) / 2, right - width));
  canvas.style.left = Math.round(x) + 'px';
  canvas.style.top = Math.round((H - height) / 2) + 'px';
  canvas.style.width = width + 'px';
  canvas.style.height = height + 'px';
}

function applyScreenMode() {
  const on = screenMode === 'on' || (screenMode === 'auto' && phoneLandscape.matches);
  root.classList.toggle('screen-only', on);
  root.classList.toggle('rotated', on && touchDevice.matches && innerHeight > innerWidth);
  layoutScreen();
  if (!on && inFullscreen()) (document.exitFullscreen || document.webkitExitFullscreen).call(document);
  if (on && !screenOn && !link.ready) showToast(statusText.textContent + ' - tap the Bluetooth button to connect', statusText.dataset.kind);
  screenOn = on;
}

phoneLandscape.addEventListener('change', () => {
  if (screenMode === 'off' && !phoneLandscape.matches) screenMode = 'auto';   // next time sideways again
  applyScreenMode();
});
window.addEventListener('resize', applyScreenMode);
if (window.visualViewport) window.visualViewport.addEventListener('resize', applyScreenMode);
document.getElementById('screen-button').addEventListener('click', () => { screenMode = 'on'; applyScreenMode(); });
document.getElementById('mini-exit').addEventListener('click', () => {
  screenMode = phoneLandscape.matches ? 'off' : 'auto';
  applyScreenMode();
});
document.getElementById('mini-full').addEventListener('click', () => {
  if (inFullscreen()) (document.exitFullscreen || document.webkitExitFullscreen).call(document);
  else (root.requestFullscreen || root.webkitRequestFullscreen).call(root);
});
miniConnect.addEventListener('click', () => {
  if (!started) return;
  if (link.ready || (link.retry && !link.userClosed)) showToast(statusText.textContent, statusText.dataset.kind);
  else connect();
});
applyScreenMode();

// ---- touch screen: mouse or finger on the canvas ----

let pointerDown = false;

function screenPoint(e) {
  const r = canvas.getBoundingClientRect();
  if (root.classList.contains('rotated')) {   // turned by 90 degrees: the screen's top edge is on the right
    return [Math.round((e.clientY - r.top) * SCREEN_W / r.height), Math.round((r.right - e.clientX) * SCREEN_H / r.width)];
  }
  return [Math.round((e.clientX - r.left) * SCREEN_W / r.width), Math.round((e.clientY - r.top) * SCREEN_H / r.height)];
}

canvas.addEventListener('pointerdown', (e) => {
  if (!started || e.button !== 0) return;
  canvas.setPointerCapture(e.pointerId);
  pointerDown = true;
  M._web_pointer(...screenPoint(e), 1);
  e.preventDefault();
});
canvas.addEventListener('pointermove', (e) => {
  if (pointerDown) M._web_pointer(...screenPoint(e), 1);
});
for (const type of ['pointerup', 'pointercancel']) {
  canvas.addEventListener(type, (e) => {
    if (!pointerDown) return;
    pointerDown = false;
    M._web_pointer(...screenPoint(e), 0);
  });
}
canvas.addEventListener('contextmenu', (e) => e.preventDefault());

// ---- footswitches: buttons below the screen and the keys 1-8 ----

const footswitch = {};   // number -> { down, timer }

function switchButton(n) {
  return document.querySelector('.fs[data-n="' + n + '"]');
}

function switchDown(n) {
  const s = footswitch[n] || (footswitch[n] = {});
  if (!started || s.down) return;
  s.down = true;
  switchButton(n).classList.add('down');
  M._web_footswitch(n, FS.PRESS);
  s.timer = setTimeout(() => { if (s.down) M._web_footswitch(n, FS.HOLD); }, HOLD_MS);
}

function switchUp(n) {
  const s = footswitch[n];
  if (!s || !s.down) return;
  s.down = false;
  clearTimeout(s.timer);
  switchButton(n).classList.remove('down');
  M._web_footswitch(n, FS.RELEASE);
}

for (const button of document.querySelectorAll('.fs')) {
  const n = Number(button.dataset.n);
  button.addEventListener('pointerdown', (e) => {
    if (e.button !== 0) return;
    button.setPointerCapture(e.pointerId);
    switchDown(n);
    e.preventDefault();
  });
  button.addEventListener('pointerup', () => switchUp(n));
  button.addEventListener('pointercancel', () => switchUp(n));
  button.addEventListener('contextmenu', (e) => e.preventDefault());
}

window.addEventListener('keydown', (e) => {
  if (e.repeat || e.ctrlKey || e.metaKey || e.altKey) return;
  if (e.key >= '1' && e.key <= '8') switchDown(Number(e.key));
});
window.addEventListener('keyup', (e) => {
  if (e.key >= '1' && e.key <= '8') switchUp(Number(e.key));
});
window.addEventListener('blur', () => {
  for (let n = 1; n <= 8; n++) switchUp(n);
});

// ---- MIDI: a MIDI input of the computer (Web MIDI) or a Bluetooth MIDI controller (Web Bluetooth) ----
// Browsers on the iPhone have no Web MIDI: there a Bluetooth MIDI controller (MC6 / MC8 Pro, WIDI) is connected
// directly, as the board does it ("Bluetooth MIDI device ..." in the MIDI list).

const MIDI_KEY = 'nano-controller:midi';   // the chosen input: its full name, or "ble:" + Bluetooth device name
const BLE_PREFIX = 'ble:';
const BLE_MIDI_SERVICE = '03b80e5a-ede8-4b33-a751-6ce34ec4c700';
const BLE_MIDI_CHAR = '7772e5db-3868-4112-a1a9-f2669d106bf3';
const midi = { access: null, input: null, wanted: '', list: [], ble: { device: null, ready: false, retry: null } };
const midiLine = document.getElementById('midi-line');

function midiWantsBle() {
  return midi.wanted.startsWith(BLE_PREFIX);
}

function midiLabel(key) {
  return key.startsWith(BLE_PREFIX) ? 'Bluetooth: ' + key.slice(BLE_PREFIX.length) : key;
}

function midiConnected() {
  return midiWantsBle() ? midi.ble.ready : !!midi.input;
}

function midiShow(message) {
  midiLine.hidden = !midi.wanted;
  if (!midi.wanted) return;
  const name = '<b>' + midiLabel(midi.wanted).replace(/[<&]/g, '') + '</b>';
  const missing = midiWantsBle() ? ' (not connected - tap it in the MIDI list)' : ' (not found - connected to this computer?)';
  midiLine.innerHTML = 'MIDI: ' + name + (midiConnected() ? '' : missing) +
    (message ? ' - received <span class="hit">' + message + '</span>' : midiConnected() ? ' - waiting for messages' : '');
}

function midiProblem(text) {
  console.warn(text);
  midiLine.hidden = false;
  midiLine.textContent = text;
  showToast(text, 'error');
}

function midiDescribe(d) {
  const type = d[0] & 0xF0, ch = (d[0] & 0x0F) + 1;
  if (type === 0xB0) return 'CC ' + d[1] + ' = ' + d[2] + ' (channel ' + ch + ')';
  if (type === 0xC0) return 'PC ' + d[1] + ' (channel ' + ch + ')';
  return Array.from(d).map((b) => b.toString(16).padStart(2, '0')).join(' ');
}

function midiMessage(status, d1, d2) {
  midiShow(midiDescribe([status, d1, d2]));
  M._web_midi_message(status, d1, d2);
}

// The screen's MIDI dialog: shown name, connected or not.
function midiReport() {
  M.ccall('web_midi_stored', null, ['string'], [midiLabel(midi.wanted)]);
  midiShow();
  M._web_midi_changed(midiConnected() ? 1 : 0);
}

function midiStore(key) {
  try {
    if (key) localStorage.setItem(MIDI_KEY, key);
    else localStorage.removeItem(MIDI_KEY);
  } catch (e) {}
}

async function midiAccess() {
  if (midi.access) return midi.access;
  if (!navigator.requestMIDIAccess) return null;   // e.g. on the iPhone
  try {
    midi.access = await navigator.requestMIDIAccess();
  } catch (e) {
    console.warn('MIDI not allowed', e);
    return null;
  }
  midi.access.addEventListener('statechange', () => { midiList(); midiAttach(); });
  return midi.access;
}

// The list of the MIDI dialog: the computer's MIDI inputs, then Bluetooth MIDI (where Web Bluetooth exists).
function midiList() {
  M._web_midi_inputs_clear();
  midi.list = [];
  if (midi.access) {
    for (const input of midi.access.inputs.values()) {
      if (input.state === 'connected' && midi.list.length < 7) midi.list.push({ input, label: input.name || 'MIDI input' });
    }
  }
  if (navigator.bluetooth) {
    const device = midi.ble.device;
    midi.list.push({ ble: true, label: device ? 'Bluetooth: ' + (device.name || 'MIDI') : 'Bluetooth MIDI device ...' });
  }
  for (const entry of midi.list) M.ccall('web_midi_input', null, ['string'], [entry.label]);
  midiReport();
}

async function midiSearch(on) {
  if (!on) return;
  await midiAccess();
  midiList();
}

function midiConnect(index) {
  const entry = midi.list[index];
  if (!entry) return;
  if (entry.ble) {
    bleMidiChoose();
    return;
  }
  bleMidiClose();
  midi.wanted = entry.input.name || '';
  midiStore(midi.wanted);
  midiAttach();
}

function midiForget() {
  bleMidiClose();
  midi.wanted = '';
  midiStore('');
  midiAttach();
}

async function midiResume() {
  try { midi.wanted = localStorage.getItem(MIDI_KEY) || ''; } catch (e) { midi.wanted = ''; }
  if (midiWantsBle()) {
    // Connect the stored Bluetooth device again without the chooser, where the browser allows it.
    if (!midi.ble.device && navigator.bluetooth && navigator.bluetooth.getDevices) {
      try {
        const name = midi.wanted.slice(BLE_PREFIX.length);
        const device = (await navigator.bluetooth.getDevices()).find((d) => (d.name || 'MIDI') === name);
        if (device) {
          bleMidiUse(device);
          return;
        }
      } catch (e) {}
    }
    midiReport();   // otherwise it is tapped once in the MIDI list
    return;
  }
  if (midi.wanted) await midiAccess();
  midiAttach();
}

// Names stored by the first version of this page were shortened to 31 characters.
function midiMatches(input) {
  return input.name === midi.wanted || (midi.wanted.length >= 31 && (input.name || '').startsWith(midi.wanted));
}

function onMidi(e) {
  const d = e.data;
  if (d.length && d[0] >= 0x80 && d[0] < 0xF0) midiMessage(d[0], d[1] || 0, d[2] || 0);   // no clock or SysEx
}

// The chosen input of the computer (Web MIDI).
function midiAttach() {
  let found = null;
  if (midi.access && midi.wanted && !midiWantsBle()) {
    for (const input of midi.access.inputs.values()) {
      if (midiMatches(input) && input.state === 'connected') found = input;
    }
  }
  if (found !== midi.input) {
    if (midi.input) midi.input.onmidimessage = null;
    midi.input = found;
    if (found) found.onmidimessage = onMidi;
  }
  midiReport();
}

// ---- Bluetooth MIDI over Web Bluetooth ----

async function bleMidiChoose() {
  let device;
  try {
    device = await navigator.bluetooth.requestDevice({ filters: [{ services: [BLE_MIDI_SERVICE] }] });
  } catch (e) {
    if (!(e.name === 'NotFoundError' && /cancel/i.test(e.message))) midiProblem('Bluetooth MIDI: ' + e.message);
    return;
  }
  bleMidiClose();
  if (midi.input) {
    midi.input.onmidimessage = null;
    midi.input = null;
  }
  midi.wanted = BLE_PREFIX + (device.name || 'MIDI');
  midiStore(midi.wanted);
  bleMidiUse(device);
}

function bleMidiUse(device) {
  midi.ble.device = device;
  device.addEventListener('gattserverdisconnected', bleMidiLost);
  bleMidiOpen();
}

async function bleMidiOpen() {
  const device = midi.ble.device;
  if (!device) return;
  try {
    const server = await device.gatt.connect();
    const service = await server.getPrimaryService(BLE_MIDI_SERVICE);
    const ch = await service.getCharacteristic(BLE_MIDI_CHAR);
    ch.addEventListener('characteristicvaluechanged', onBleMidi);   // the same listener is added only once
    await ch.startNotifications();
    if (device !== midi.ble.device) return;   // forgotten meanwhile
    midi.ble.ready = true;
    showToast('MIDI: ' + (device.name || 'Bluetooth') + ' connected', 'ok');
  } catch (e) {
    console.warn('Bluetooth MIDI not connected', e);
    if (device === midi.ble.device) bleMidiRetry();
  }
  midiList();
}

function bleMidiLost() {
  midi.ble.ready = false;
  midiReport();
  if (midiWantsBle() && midi.ble.device) bleMidiRetry();
}

function bleMidiRetry() {
  clearTimeout(midi.ble.retry);
  midi.ble.retry = setTimeout(() => {
    if (midiWantsBle() && midi.ble.device && !midi.ble.ready) bleMidiOpen();
  }, 3000);
}

function bleMidiClose() {
  const device = midi.ble.device;
  clearTimeout(midi.ble.retry);
  midi.ble = { device: null, ready: false, retry: null };
  if (device) {
    device.removeEventListener('gattserverdisconnected', bleMidiLost);
    if (device.gatt.connected) device.gatt.disconnect();
  }
}

function onBleMidi(event) {
  const v = event.target.value;
  parseBleMidi(new Uint8Array(v.buffer, v.byteOffset, v.byteLength), midiMessage);
}

// BLE MIDI packet: header (bit 7 set, timestamp high), then messages, each preceded by a timestamp byte (bit 7 set)
// unless it continues with running status. SysEx and system messages are skipped. (As parse_packet in midi_ble.c.)
function parseBleMidi(p, emit) {
  const n = p.length;
  if (n < 2 || !(p[0] & 0x80)) return;
  let running = 0, sysex = false, i = 1;
  while (i < n) {
    if (p[i] & 0x80) {                       // timestamp
      if (++i >= n) break;
      if (p[i] & 0x80) {                     // status
        const status = p[i++];
        if (status === 0xF0) { sysex = true; continue; }
        if (status === 0xF7) { sysex = false; continue; }
        if (status >= 0xF8) continue;        // real-time (clock etc.)
        running = status;
        sysex = false;
      }
    }
    if (sysex || !running) { i++; continue; }   // inside a SysEx or no status yet: skip the data byte
    const type = running & 0xF0;
    const count = type === 0xC0 || type === 0xD0 ? 1 : type >= 0x80 && type <= 0xE0 ? 2 : -1;
    if (count < 0 || i + count > n) { running = 0; i++; continue; }
    const d1 = p[i], d2 = count === 2 ? p[i + 1] : 0;
    i += count;
    if ((d1 | d2) & 0x80) continue;
    emit(running, d1, d2);
  }
}
