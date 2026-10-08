// Capture and cab library of the Nano Cortex (RetrieveLibraryContent 76 / 77).
// Items keep the index of their position in the Nano's list: that is how a slot load addresses them.
#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

// Capture categories as in the editor: AMP = amp head or combo alone, AMP_CAB = amp captured with its cab.
typedef enum { LIB_ALL, LIB_AMP, LIB_AMP_CAB, LIB_CAB, LIB_PEDAL, LIB_OTHER, LIB_CATEGORY_COUNT } lib_category_t;

typedef struct {
    char name[64];
    uint16_t index;      // position in the Nano's factory or user list
    uint8_t user;        // 0 = factory, 1 = user
    uint8_t category;    // lib_category_t (captures only)
    uint8_t kind;        // lib_kind_t: the capture type as in the editor (captures only)
} lib_item_t;

// Capture types as the Nano names them (field 6 of a library record), in the editor's order.
typedef enum { LIB_KIND_AMP_HEAD, LIB_KIND_AMP_COMBO, LIB_KIND_AMP_CAB, LIB_KIND_CAB, LIB_KIND_PEDAL, LIB_KIND_OVERDRIVE,
               LIB_KIND_FUZZ, LIB_KIND_COMPRESSOR, LIB_KIND_OTHER, LIB_KIND_COUNT } lib_kind_t;

typedef struct {
    lib_item_t *items;   // alphabetical
    int count;
} lib_list_t;

typedef struct {
    lib_list_t captures;
    lib_list_t cabs;
} nano_library_t;

extern const char *const LIB_CATEGORY_NAMES[LIB_CATEGORY_COUNT];

// Request payload for type 76 (factory and user captures and IRs).
size_t nano_library_request(uint8_t *out);

// Parses a type 77 reply into a newly allocated library (PSRAM). NULL on failure.
nano_library_t *nano_library_parse(const uint8_t *payload, size_t len);

// SetCaptureSlotContent (80) / SetCabinetSlotContent (78): load a library item into slot (1-based).
// Returns 0 if the name is too long for one message.
size_t nano_capture_load(int slot, const lib_item_t *item, uint8_t *out, size_t max);
size_t nano_cab_load(int slot, const lib_item_t *item, uint8_t *out, size_t max);
