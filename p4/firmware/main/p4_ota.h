// P4 board: a new firmware over Wi-Fi (the upload page at http://nano-board.local, PUT /api/firmware).
//
// The new firmware is written into the other of the two firmware places (partitions.csv) while the board goes on
// working; only a file made for this board is taken (the right chip and the project name nano_controller_p4 - the
// small board's firmware, a test project or anything else is refused before anything is written), and its checksum
// is checked at the end. After the restart the new firmware has to show it works: once it has run for 30 s with the
// screen answering, it is kept (p4_ota_start). If it does not get that far (crash, hang, watchdog), the next start
// goes back to the firmware before - the board cannot be left without a working firmware by an update.
#pragma once

#include <stdbool.h>
#include <stddef.h>

typedef struct p4_ota p4_ota_t;

// At the start: logs which firmware runs, and keeps a new one once it has worked for 30 s.
void p4_ota_start(void);

// An update of `total` bytes. NULL and a reason in `why` if it cannot be taken.
p4_ota_t *p4_ota_open(size_t total, char *why, size_t why_len);
// The next piece of the file. false (and a reason) ends the update; the other place stays as it was not chosen.
bool p4_ota_write(p4_ota_t *ota, const void *data, size_t len, char *why, size_t why_len);
// The whole file is there: checked, and chosen for the next start. Frees `ota` either way.
bool p4_ota_finish(p4_ota_t *ota, char *why, size_t why_len);
void p4_ota_abort(p4_ota_t *ota);
// Restarts the board in `ms` (after the reply has gone out).
void p4_ota_restart_soon(int ms);
// The version of the new firmware of the last update (empty if none) - for the reply.
const char *p4_ota_new_version(void);
