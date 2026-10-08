// Browser build of midi_ble: the MIDI inputs of the computer through Web MIDI (web/app.js), for example an MC6
// over USB or a WIDI paired with the computer. The chosen input is stored in the browser and used again by itself.
// app.js keeps the full names (they can be longer than the 31 characters kept here for the screen).
#include "midi_ble.h"

#include <emscripten.h>
#include <stdio.h>
#include <string.h>

#include "esp_log.h"

static const char *TAG = "midi";

static midi_message_cb s_on_message;
static midi_change_cb s_on_change;
static bool s_allowed = true, s_connected;
static char s_names[MIDI_MAX_DEVICES][32];
static int s_count;
static char s_stored[32];

void web_platform_run(void);

EM_JS(void, js_midi_search, (int on), { Module.midiSearch(on); });
EM_JS(void, js_midi_connect, (int index), { Module.midiConnect(index); });   // stores the input's full name
EM_JS(void, js_midi_forget, (void), { Module.midiForget(); });
EM_JS(void, js_midi_resume, (void), { Module.midiResume(); });              // the stored input, when it is there
EM_JS(int, js_midi_stored, (char *out, int size), {   // shown name of the stored input (as midiLabel in app.js)
    let name = "";
    try { name = localStorage.getItem("nano-controller:midi") || ""; } catch (e) {}
    if (name.startsWith("ble:")) name = "Bluetooth: " + name.slice(4);
    stringToUTF8(name, out, size);
    return name.length;
});

static void use_stored(void)
{
    if (s_allowed && s_stored[0]) js_midi_resume();
}

void midi_ble_start(midi_message_cb on_message, midi_change_cb on_change)
{
    s_on_message = on_message;
    s_on_change = on_change;
    js_midi_stored(s_stored, sizeof(s_stored));
}

void midi_ble_search(bool on)
{
    js_midi_search(on);
}

void midi_ble_allow(bool allowed)
{
    if (allowed == s_allowed) return;
    s_allowed = allowed;
    if (allowed) use_stored();   // an existing connection stays when not allowed (as with Bluetooth)
}

int midi_ble_devices(midi_device_t *out, int max)
{
    int n = 0;
    for (int i = 0; i < s_count && n < max; i++, n++) {
        snprintf(out[n].name, sizeof(out[n].name), "%s", s_names[i]);
        out[n].rssi = 0;
        out[n].remembered = s_stored[0] && !strcmp(s_names[i], s_stored);
    }
    return n;
}

bool midi_ble_connect(int index)
{
    if (index < 0 || index >= s_count) return false;
    snprintf(s_stored, sizeof(s_stored), "%s", s_names[index]);
    js_midi_connect(index);
    return true;
}

void midi_ble_forget(void)
{
    s_stored[0] = 0;
    js_midi_forget();
}

bool midi_ble_status(char *name, size_t size)
{
    snprintf(name, size, "%s", s_stored);
    return s_connected;
}

// ---- from app.js ----

// The name the screen shows for the chosen input (it may differ from the list entry, e.g. a Bluetooth device).
EMSCRIPTEN_KEEPALIVE void web_midi_stored(const char *name)
{
    snprintf(s_stored, sizeof(s_stored), "%s", name);
}

EMSCRIPTEN_KEEPALIVE void web_midi_inputs_clear(void)
{
    s_count = 0;
}

EMSCRIPTEN_KEEPALIVE void web_midi_input(const char *name)
{
    if (s_count < MIDI_MAX_DEVICES) snprintf(s_names[s_count++], sizeof(s_names[0]), "%s", name);
}

// The list is complete, or the stored input came or went.
EMSCRIPTEN_KEEPALIVE void web_midi_changed(int connected)
{
    if (s_connected != (connected != 0)) ESP_LOGI(TAG, "%s %s", s_stored, connected ? "connected" : "not available");
    s_connected = connected != 0;
    if (s_on_change) s_on_change();
    web_platform_run();
}

EMSCRIPTEN_KEEPALIVE void web_midi_message(int status, int data1, int data2)
{
    if (s_on_message && status >= 0x80 && status <= 0xEF) s_on_message((uint8_t)status, (uint8_t)data1, (uint8_t)data2);
    web_platform_run();
}

// The page has asked for MIDI access (needs the page to be open once): use the stored input.
EMSCRIPTEN_KEEPALIVE void web_midi_ready(void)
{
    use_stored();
}
