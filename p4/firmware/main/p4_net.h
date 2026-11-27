// P4 board: Wi-Fi only for sending jam tracks from the Mac to the board (the ESP32-C6 on the 7B does the radio,
// esp_hosted over SDIO). Off by default and after every start - on stage the board needs no Wi-Fi.
//
// On: with a network set (the jam player's "Network" button: name and password on the screen's keyboard) the board
// joins it; without one - or if it cannot - it opens its own network "Nano Board" with a password shown on the
// screen. Either way it answers at http://nano-board.local (and at its address): a page to drop audio files on (the
// browser on the Mac decodes them, measures them as the Jam Player does and sends 16-bit WAV), and a small API
// (PUT /api/tracks/<name>.wav, PUT /api/covers/<name>.jpg, GET /api/tracks, POST /api/trims, POST /api/move,
// POST /api/sources, POST /api/done; <name> "Album/Title" or "Title", 2026-11-27) for the Jam Player app, and
// PUT /api/firmware for a new firmware (p4_ota.h).
//
// STATUS: builds; not yet run on hardware (C6 firmware, SDIO pins 18/19/14-17 and reset 54 are esp_hosted's defaults
// for the P4 - to check on the board).
#pragma once

#include <stdbool.h>
#include <stdint.h>

typedef enum { P4_NET_OFF, P4_NET_STARTING, P4_NET_JOINING, P4_NET_STATION, P4_NET_AP, P4_NET_FAILED } p4_net_state_t;

typedef struct {
    p4_net_state_t state;
    char ssid[33];              // the network joined / set, or the board's own
    char addr[48];              // "192.168.1.23"
    char ap_pass[12];           // the board's own network: its password
    int received;               // files received since Wi-Fi went on
    char receiving[64];         // the file coming in now ("" = none)
    int percent;
} p4_net_status_t;

void p4_net_enable(bool on);
bool p4_net_enabled(void);
void p4_net_set_network(const char *ssid, const char *password);   // "" = forget it (the board's own network)
void p4_net_status(p4_net_status_t *out);
void p4_net_text(char kind, const char *text);   // the screen's keyboard: 'W' network name, then 'K' password

// The networks around (the Wi-Fi part looks for them for about 2 s; with Wi-Fi off it is switched on for that and
// off again). Blocking - call it from a task of its own. Strongest first, each name once.
#define P4_NET_SCAN_MAX 16
typedef struct {
    char ssid[33];
    int8_t rssi;                // dBm
    bool secure;                // a password is needed
} p4_net_ap_t;
void p4_net_scan(void);
bool p4_net_scanning(void);
int p4_net_scan_results(p4_net_ap_t *out, int max);   // the last scan's networks (0 before the first)
