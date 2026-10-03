#include "library.h"

#include <stdlib.h>
#include <string.h>
#include <strings.h>

#include "esp_heap_caps.h"

const char *const LIB_CATEGORY_NAMES[LIB_CATEGORY_COUNT] = { "ALL", "AMP", "AMP+CAB", "CAB", "PEDAL", "OTHER" };

// Minimal protobuf walk over a buffer: calls back for every length-delimited field.
static bool next_field(const uint8_t **p, const uint8_t *end, uint32_t *field, uint32_t *wire,
                       const uint8_t **data, size_t *len, uint64_t *value)
{
    uint64_t tag = 0, v = 0;
    int shift = 0;
    while (*p < end) {
        uint8_t b = *(*p)++;
        tag |= (uint64_t)(b & 0x7f) << shift;
        if (!(b & 0x80)) break;
        shift += 7;
    }
    *field = (uint32_t)(tag >> 3);
    *wire = (uint32_t)(tag & 7);
    if (*wire == 0 || *wire == 2) {
        shift = 0;
        while (*p < end) {
            uint8_t b = *(*p)++;
            v |= (uint64_t)(b & 0x7f) << shift;
            if (!(b & 0x80)) break;
            shift += 7;
        }
        if (*wire == 0) { *value = v; return true; }
        if (v > (uint64_t)(end - *p)) return false;
        *data = *p;
        *len = (size_t)v;
        *p += v;
        return true;
    }
    size_t skip = *wire == 1 ? 8 : *wire == 5 ? 4 : 0;
    if (!skip || skip > (size_t)(end - *p)) return false;
    *p += skip;
    return true;
}

static void copy_name(char *dst, size_t size, const uint8_t *src, size_t len)
{
    if (len >= size) len = size - 1;
    memcpy(dst, src, len);
    dst[len] = 0;
}

static lib_category_t category_of(const char *type)
{
    if (!strcmp(type, "amp_head") || !strcmp(type, "amp_combo")) return LIB_AMP;
    if (!strcmp(type, "amp_and_cab")) return LIB_AMP_CAB;
    if (!strcmp(type, "cab")) return LIB_CAB;
    if (!strcmp(type, "pedal") || !strcmp(type, "overdrive") || !strcmp(type, "fuzz") || !strcmp(type, "compressor")) return LIB_PEDAL;
    return LIB_OTHER;
}

// NeuralCaptureSummaryRecord { 2: name, 6: captureType, 13: isCorrupted }. Returns false for corrupted ones.
static bool parse_capture(const uint8_t *data, size_t len, lib_item_t *item)
{
    const uint8_t *p = data, *end = data + len, *d = NULL;
    uint32_t field, wire;
    size_t n = 0;
    uint64_t value = 0;
    char type[24] = "";
    bool corrupted = false;
    item->name[0] = 0;
    while (p < end && next_field(&p, end, &field, &wire, &d, &n, &value)) {
        if (field == 2 && wire == 2) copy_name(item->name, sizeof(item->name), d, n);
        else if (field == 6 && wire == 2) copy_name(type, sizeof(type), d, n);
        else if (field == 13 && wire == 0) corrupted = value != 0;
    }
    item->category = category_of(type);
    return !corrupted && item->name[0];
}

static int by_name(const void *a, const void *b)
{
    return strcasecmp(((const lib_item_t *)a)->name, ((const lib_item_t *)b)->name);
}

// 3 factory captures[], 4 factory IR names[], 5 user captures[], 6 user IR names[].
nano_library_t *nano_library_parse(const uint8_t *payload, size_t len)
{
    int counts[7] = { 0 };
    const uint8_t *p = payload, *end = payload + len, *d;
    uint32_t field, wire;
    size_t n;
    uint64_t value;
    while (p < end && next_field(&p, end, &field, &wire, &d, &n, &value)) {
        if (wire == 2 && field >= 3 && field <= 6) counts[field]++;
    }

    nano_library_t *lib = heap_caps_calloc(1, sizeof(*lib), MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if (!lib) return NULL;
    int captures = counts[3] + counts[5], cabs = counts[4] + counts[6];
    lib->captures.items = captures ? heap_caps_calloc(captures, sizeof(lib_item_t), MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT) : NULL;
    lib->cabs.items = cabs ? heap_caps_calloc(cabs, sizeof(lib_item_t), MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT) : NULL;
    if ((captures && !lib->captures.items) || (cabs && !lib->cabs.items)) {
        free(lib->captures.items);
        free(lib->cabs.items);
        free(lib);
        return NULL;
    }

    int index[7] = { 0 };
    p = payload;
    while (p < end && next_field(&p, end, &field, &wire, &d, &n, &value)) {
        if (wire != 2 || field < 3 || field > 6) continue;
        uint16_t position = (uint16_t)index[field]++;
        if (field == 3 || field == 5) {
            lib_item_t *item = &lib->captures.items[lib->captures.count];
            if (!parse_capture(d, n, item)) continue;
            item->index = position;
            item->user = field == 5;
            lib->captures.count++;
        } else {
            lib_item_t *item = &lib->cabs.items[lib->cabs.count];
            copy_name(item->name, sizeof(item->name), d, n);
            if (!item->name[0]) continue;
            item->index = position;
            item->user = field == 6;
            item->category = LIB_CAB;
            lib->cabs.count++;
        }
    }
    qsort(lib->captures.items, lib->captures.count, sizeof(lib_item_t), by_name);
    qsort(lib->cabs.items, lib->cabs.count, sizeof(lib_item_t), by_name);
    return lib;
}

size_t nano_library_request(uint8_t *out)
{
    static const uint8_t req[] = { 0x18, 0x01, 0x20, 0x01, 0x28, 0x01, 0x30, 0x01 };
    memcpy(out, req, sizeof(req));
    return sizeof(req);
}

static size_t put_varint(uint8_t *out, uint32_t v)
{
    size_t n = 0;
    while (v > 0x7f) {
        out[n++] = (uint8_t)(v & 0x7f) | 0x80;
        v >>= 7;
    }
    out[n++] = (uint8_t)v;
    return n;
}

// { 3: slot (0-based), 4: factory id | 5: user id } with id { 1: index, 2: name }.
static size_t slot_load(int slot, const lib_item_t *item, uint8_t *out, size_t max)
{
    uint8_t id[96];
    size_t name_len = strlen(item->name), n = 0;
    id[n++] = 0x08;
    n += put_varint(id + n, item->index);
    id[n++] = 0x12;
    n += put_varint(id + n, (uint32_t)name_len);
    if (n + name_len > sizeof(id)) return 0;
    memcpy(id + n, item->name, name_len);
    n += name_len;

    size_t m = 0;
    if (n + 8 > max) return 0;
    out[m++] = 0x18;
    m += put_varint(out + m, (uint32_t)(slot - 1));
    out[m++] = item->user ? 0x2A : 0x22;
    m += put_varint(out + m, (uint32_t)n);
    memcpy(out + m, id, n);
    return m + n;
}

size_t nano_capture_load(int slot, const lib_item_t *item, uint8_t *out, size_t max)
{
    return slot_load(slot, item, out, max);
}

size_t nano_cab_load(int slot, const lib_item_t *item, uint8_t *out, size_t max)
{
    return slot_load(slot, item, out, max);
}
