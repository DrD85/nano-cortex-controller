#pragma once

#include <stdbool.h>

// A firmware for the ESP32-C6 placed into ota_1 (see p4_c6_update.c): sent to the C6 once, then the board starts
// again. running_version: the C6's version as "1.2.3", or "" when it reports none. False when nothing was sent.
bool p4_c6_update_if_staged(const char *running_version);
