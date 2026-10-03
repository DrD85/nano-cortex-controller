#include "nano_state.h"

#include <string.h>

const char *const NANO_FX_SLOT_NAMES[NANO_FX_SLOTS] = { "Pre FX 1", "Pre FX 2", "Post FX 1", "Post FX 2", "Post FX 3" };

// ---- minimal protobuf reader ----

typedef struct {
    const uint8_t *p, *end;
} pb_reader_t;

typedef struct {
    uint32_t field, wire;
    uint64_t value;          // wire type 0
    const uint8_t *data;     // wire types 1, 2, 5
    size_t len;
} pb_field_t;

static bool pb_varint(pb_reader_t *r, uint64_t *out)
{
    uint64_t v = 0;
    for (int shift = 0; shift < 64 && r->p < r->end; shift += 7) {
        uint8_t b = *r->p++;
        v |= (uint64_t)(b & 0x7f) << shift;
        if (!(b & 0x80)) {
            *out = v;
            return true;
        }
    }
    return false;
}

static bool pb_next(pb_reader_t *r, pb_field_t *f)
{
    uint64_t tag, n;
    if (r->p >= r->end || !pb_varint(r, &tag)) return false;
    f->field = (uint32_t)(tag >> 3);
    f->wire = (uint32_t)(tag & 7);
    f->value = 0;
    f->data = NULL;
    f->len = 0;
    switch (f->wire) {
    case 0:
        return pb_varint(r, &f->value);
    case 1:
    case 5:
        n = f->wire == 1 ? 8 : 4;
        break;
    case 2:
        if (!pb_varint(r, &n)) return false;
        break;
    default:
        return false;
    }
    if (n > (uint64_t)(r->end - r->p)) return false;
    f->data = r->p;
    f->len = (size_t)n;
    r->p += n;
    return true;
}

// Value of a varint field, also when it arrives as length-delimited bytes holding a varint.
static uint64_t field_number(const pb_field_t *f)
{
    if (f->wire == 0) return f->value;
    if (f->wire == 2) {
        pb_reader_t r = { f->data, f->data + f->len };
        uint64_t v;
        if (pb_varint(&r, &v)) return v;
    }
    return 0;
}

static void copy_string(char *dst, size_t size, const uint8_t *src, size_t len)
{
    if (len >= size) len = size - 1;
    memcpy(dst, src, len);
    dst[len] = 0;
}

// First string `field` inside a nested message.
static bool nested_string(const uint8_t *data, size_t len, uint32_t field, char *dst, size_t size)
{
    pb_reader_t r = { data, data + len };
    pb_field_t f;
    while (pb_next(&r, &f)) {
        if (f.field == field && f.wire == 2) {
            copy_string(dst, size, f.data, f.len);
            return true;
        }
    }
    return false;
}

uint64_t nano_field_varint(const uint8_t *payload, size_t len, uint32_t field)
{
    pb_reader_t r = { payload, payload + len };
    pb_field_t f;
    while (pb_next(&r, &f)) if (f.field == field) return field_number(&f);
    return 0;
}

// ---- StateResponse ----
// 9 bankSelector (1-5), 11 neuralCaptureSelector (position 1-5 in the bank, 0 = bypass), 12 cabinetSelector,
// 17 captures[] { 2 name }, 19 cabinets[] { 1 short name } (full state only),
// 13 currentPresetIndex, 14/15/38/39 presets on slots A1/B1/A2/B2, 18 presets[] { 1 name },
// 31 fxBypass (5 bytes, 0 = on), 32 current capture { 2 name }, 33 current cab { 2 short name },
// 41 currentPresetDirty, 46 tunerBaseFrequency (float32), 48-52 FX model type per slot.
// The Nano omits fields whose value is 0.
bool nano_payload_complete(const uint8_t *payload, size_t len)
{
    pb_reader_t r = { payload, payload + len };
    pb_field_t f;
    while (pb_next(&r, &f)) {}
    return r.p == r.end;
}

bool nano_state_apply(nano_state_t *st, const uint8_t *payload, size_t len)
{
    int preset_index = 0, slots[4] = { 0 }, names = 0, captures = 0, cabs = 0;
    int bank = 1, capture_position = 0, cab_selector = 0;
    bool dirty = false, fx_seen = false, names_seen = false, capture_seen = false, cab_seen = false;
    bool capture_names_seen = false, cab_names_seen = false;
    uint32_t types[NANO_FX_SLOTS] = { 0 };
    char capture[sizeof(st->capture)] = "", cab[sizeof(st->cab)] = "";

    pb_reader_t r = { payload, payload + len };
    pb_field_t f;
    while (pb_next(&r, &f)) {
        switch (f.field) {
        case 9: bank = (int)field_number(&f); break;
        case 11: capture_position = (int)field_number(&f); break;
        case 12: cab_selector = (int)field_number(&f); break;
        case 17:
            if (f.wire != 2) break;
            capture_names_seen = true;
            if (captures < NANO_CAPTURE_SLOTS) {
                char *dst = st->capture_names[captures++];
                if (!nested_string(f.data, f.len, 2, dst, sizeof(st->capture_names[0]))) dst[0] = 0;
            }
            break;
        case 19:
            if (f.wire != 2) break;
            cab_names_seen = true;
            if (cabs < NANO_CAB_SLOTS) {
                char *dst = st->cab_names[cabs++];
                if (!nested_string(f.data, f.len, 1, dst, sizeof(st->cab_names[0]))) dst[0] = 0;
            }
            break;
        case 13: preset_index = (int)field_number(&f); break;
        case 14: slots[0] = (int)field_number(&f); break;
        case 15: slots[1] = (int)field_number(&f); break;
        case 38: slots[2] = (int)field_number(&f); break;
        case 39: slots[3] = (int)field_number(&f); break;
        case 41: dirty = field_number(&f) != 0; break;
        case 46:
            if (f.wire == 5) {
                float hz;
                memcpy(&hz, f.data, sizeof(hz));
                if (hz >= 400 && hz <= 480) st->tuner_base_hz = hz;
            }
            break;
        case 31:
            if (f.wire == 2 && f.len >= NANO_FX_SLOTS) {
                for (int i = 0; i < NANO_FX_SLOTS; i++) st->fx_on[i] = f.data[i] == 0;
                fx_seen = true;
            }
            break;
        case 48: case 49: case 50: case 51: case 52:
            types[f.field - 48] = (uint32_t)field_number(&f);
            break;
        case 32:
            if (f.wire == 2) {
                nested_string(f.data, f.len, 2, capture, sizeof(capture));
                capture_seen = true;
            }
            break;
        case 33:
            if (f.wire == 2) {
                nested_string(f.data, f.len, 2, cab, sizeof(cab));
                cab_seen = true;
            }
            break;
        case 18:
            if (f.wire != 2) break;
            names_seen = true;
            if (names < NANO_PRESETS) {
                char *dst = st->preset_names[names++];
                if (!nested_string(f.data, f.len, 1, dst, sizeof(st->preset_names[0]))) dst[0] = 0;
            }
            break;
        default:
            break;
        }
    }

    st->current_preset = preset_index + 1;
    memcpy(st->slot_preset, slots, sizeof(slots));
    st->dirty = dirty;
    if (bank < 1 || bank > 5) bank = 1;
    st->capture_slot = capture_position > 0 ? (bank - 1) * 5 + capture_position : 0;
    st->cab_slot = cab_selector >= 0 && cab_selector <= NANO_CAB_SLOTS ? cab_selector : 0;
    if (capture_names_seen) {
        for (int i = captures; i < NANO_CAPTURE_SLOTS; i++) st->capture_names[i][0] = 0;
    }
    if (cab_names_seen) {
        for (int i = cabs; i < NANO_CAB_SLOTS; i++) st->cab_names[i][0] = 0;
    }
    if (fx_seen) {
        st->fx_known = true;
        memcpy(st->fx_type, types, sizeof(types));
    }
    if (capture_seen) strcpy(st->capture, capture);
    if (cab_seen) strcpy(st->cab, cab);
    if (names_seen) {
        for (int i = names; i < NANO_PRESETS; i++) st->preset_names[i][0] = 0;
        st->names_loaded = true;
    }
    return names_seen;
}

// SetPresetSlotsRequest from the device: 4 currentPresetIndex, 5-8 slots A1/B1/A2/B2.
void nano_state_apply_preset_change(nano_state_t *st, const uint8_t *payload, size_t len)
{
    st->current_preset = (int)nano_field_varint(payload, len, 4) + 1;
    for (int i = 0; i < 4; i++) st->slot_preset[i] = (int)nano_field_varint(payload, len, 5 + i);
}

// ---- requests ----

static size_t put_varint(uint8_t *out, uint64_t v)
{
    size_t n = 0;
    while (v > 0x7f) {
        out[n++] = (uint8_t)(v & 0x7f) | 0x80;
        v >>= 7;
    }
    out[n++] = (uint8_t)v;
    return n;
}

// StateRequest { 1: action READ } -> everything, including all preset names.
size_t nano_full_state_request(uint8_t *out)
{
    out[0] = 0x08; out[1] = 0x03;
    return 2;
}

// StateRequest { 1: READ, 3: 1, 4: 1, 5: 1 } -> the current preset (as the editor requests it).
size_t nano_current_state_request(uint8_t *out)
{
    static const uint8_t req[] = { 0x08, 0x03, 0x18, 0x01, 0x20, 0x01, 0x28, 0x01 };
    memcpy(out, req, sizeof(req));
    return sizeof(req);
}

// SetPresetSlotsRequest { 3 context = 0, 4 presetIndex, 5-8 slots = -1 (unchanged),
// 9 currentSlot = footswitch slot holding this preset, else 4 (other) }.
size_t nano_preset_change(const nano_state_t *st, int preset, uint8_t *out)
{
    int index = preset - 1, current_slot = 4;
    for (int i = 0; i < 4; i++) {
        if (st->slot_preset[i] == index) {
            current_slot = i;
            break;
        }
    }
    size_t n = 0;
    out[n++] = 0x18; out[n++] = 0x00;
    out[n++] = 0x20; n += put_varint(out + n, (uint64_t)index);
    for (int field = 5; field <= 8; field++) {
        out[n++] = (uint8_t)(field << 3);
        n += put_varint(out + n, UINT64_MAX);   // int32 -1 as a sign-extended 10-byte varint
    }
    out[n++] = 0x48; out[n++] = (uint8_t)current_slot;
    return n;
}

// BypassControl { 1: action 1, 3: slot (FX 1-5 = 4-8), 4: 0 = on, 1 = off }.
size_t nano_fx_bypass(int slot, bool on, uint8_t *out)
{
    out[0] = 0x08; out[1] = 0x01;
    out[2] = 0x18; out[3] = (uint8_t)(4 + slot);
    out[4] = 0x20; out[5] = on ? 0x00 : 0x01;
    return 6;
}

static size_t put_int32(uint8_t *out, int v)
{
    return put_varint(out, v < 0 ? UINT64_MAX : (uint64_t)v);   // negative int32 values are sign-extended
}

size_t nano_preset_changed_notice(const nano_state_t *st, uint8_t *out)
{
    int index = st->current_preset - 1, current_slot = 4;
    size_t n = 0;
    out[n++] = 0x18; out[n++] = 0x00;
    out[n++] = 0x20; n += put_varint(out + n, (uint64_t)index);
    for (int i = 0; i < 4; i++) {
        out[n++] = (uint8_t)((5 + i) << 3);
        n += put_int32(out + n, st->slot_preset[i]);
        if (st->slot_preset[i] == index && current_slot == 4) current_slot = i;
    }
    out[n++] = 0x48; out[n++] = (uint8_t)current_slot;
    return n;
}

// SetPresetSlotsResponse { 4: status = success }. Without it the Nano waits in a pending preset change.
size_t nano_preset_change_ack(uint8_t *out)
{
    out[0] = 0x20; out[1] = 0x01;
    return 2;
}

// TunerMode { 4: tunerMode, 5: baseFrequency (float32), 6: liveTuner, 7: muted }. liveTuner = true is what
// switches the Nano into tuner mode when the tuner is opened from outside.
size_t nano_tuner_mode(bool on, float base_hz, uint8_t *out)
{
    size_t n = 0;
    out[n++] = 0x20; out[n++] = on ? 1 : 0;
    if (!on) return n;
    out[n++] = 0x2D;
    memcpy(out + n, &base_hz, sizeof(base_hz));   // little-endian float32
    n += sizeof(base_hz);
    out[n++] = 0x30; out[n++] = 1;
    out[n++] = 0x38; out[n++] = 0;
    return n;
}

// FxValue { 1: action 1, 3: slot 0-4, 4: parameter index, 5: value 0-1 (float32) }.
size_t nano_fx_param(int slot, int param, float normalized, uint8_t *out)
{
    size_t n = 0;
    out[n++] = 0x08; out[n++] = 0x01;
    out[n++] = 0x18; out[n++] = (uint8_t)slot;
    out[n++] = 0x20; out[n++] = (uint8_t)param;
    out[n++] = 0x2D;
    memcpy(out + n, &normalized, sizeof(normalized));
    return n + sizeof(normalized);
}

// RetrieveExpressionAssignments { 1: READ, 3: presetIndex }.
size_t nano_exp_request(int preset, uint8_t *out)
{
    out[0] = 0x08; out[1] = 0x03; out[2] = 0x18;
    return 3 + put_varint(out + 3, (uint64_t)(preset - 1));
}

// Expression reply: the Amount range of FX slot s is field 7 + s,
// { 1: inverted, 2: min, 3: max } with values 0-255.
bool nano_exp_amount_range(const uint8_t *payload, size_t len, int slot, float pos[2])
{
    pb_reader_t r = { payload, payload + len };
    pb_field_t f;
    while (pb_next(&r, &f)) {
        if (f.field != (uint32_t)(7 + slot) || f.wire != 2) continue;
        bool inverted = nano_field_varint(f.data, f.len, 1) != 0;
        float min = nano_field_varint(f.data, f.len, 2) / 255.0f, max = nano_field_varint(f.data, f.len, 3) / 255.0f;
        pos[0] = inverted ? max : min;
        pos[1] = inverted ? min : max;
        return true;
    }
    return false;
}

// SaveExpAssignments { 3: presetIndex, 4-22: assignments }: the reply carries the same entries one field
// number lower (Amount range of FX slot s: reply 7 + s, save 8 + s). Inverted = Pos 1 above Pos 2.
size_t nano_exp_save_amount_range(const uint8_t *reply, size_t reply_len, int preset, int slot,
                                  uint8_t pos1, uint8_t pos2, uint8_t *out, size_t max)
{
    uint8_t range[8];
    size_t m = 0;
    range[m++] = 0x08; range[m++] = pos1 > pos2;
    range[m++] = 0x10; m += put_varint(range + m, pos1 > pos2 ? pos2 : pos1);
    range[m++] = 0x18; m += put_varint(range + m, pos1 > pos2 ? pos1 : pos2);

    size_t n = 0;
    out[n++] = 0x18;
    n += put_varint(out + n, (uint64_t)(preset - 1));
    for (uint32_t field = 3; field <= 21; field++) {
        const uint8_t *data = NULL;
        size_t len = 0;
        if (field == (uint32_t)(7 + slot)) {
            data = range;
            len = m;
        } else {
            pb_reader_t r = { reply, reply + reply_len };
            pb_field_t f;
            while (pb_next(&r, &f)) {
                if (f.field == field && f.wire == 2) {
                    data = f.data ? f.data : range;   // empty entry: no bytes to copy
                    len = f.len;
                    break;
                }
            }
            if (!data) continue;
        }
        if (n + 4 + len > max) return 0;
        n += put_varint(out + n, ((uint64_t)(field + 1) << 3) | 2);
        n += put_varint(out + n, len);
        memcpy(out + n, data, len);
        n += len;
    }
    return n;
}

// TunerMetering { 4: note, 5: deviation in cents (float32, sometimes double), 7: centered }.
void nano_tuner_reading(const uint8_t *payload, size_t len, nano_tuner_reading_t *out)
{
    memset(out, 0, sizeof(*out));
    pb_reader_t r = { payload, payload + len };
    pb_field_t f;
    while (pb_next(&r, &f)) {
        if (f.field == 4 && f.wire == 2) copy_string(out->note, sizeof(out->note), f.data, f.len);
        else if (f.field == 5 && f.wire == 5) memcpy(&out->cents, f.data, sizeof(float));
        else if (f.field == 5 && f.wire == 1) { double d; memcpy(&d, f.data, sizeof(d)); out->cents = (float)d; }
        else if (f.field == 7) out->centered = field_number(&f) != 0;
    }
    out->valid = out->note[0] != 0;
}

// { 1: action UPDATE, 3: presetIndex, <field>: name } - SavePreset uses field 5, RenamePreset field 4.
static size_t preset_name_message(int preset, int name_field, const char *name, uint8_t *out, size_t max)
{
    size_t len = strlen(name), n = 0;
    if (len + 8 > max) return 0;
    out[n++] = 0x08; out[n++] = 0x01;
    out[n++] = 0x18; n += put_varint(out + n, (uint64_t)(preset - 1));
    out[n++] = (uint8_t)(name_field << 3 | 2);
    n += put_varint(out + n, len);
    memcpy(out + n, name, len);
    return n + len;
}

size_t nano_save_preset(int preset, const char *name, uint8_t *out, size_t max)
{
    return preset_name_message(preset, 5, name, out, max);
}

size_t nano_rename_preset(int preset, const char *name, uint8_t *out, size_t max)
{
    return preset_name_message(preset, 4, name, out, max);
}

// SelectorValue { 3: selector (1 = capture, 3 = cabinet), 4: value }.
static size_t selector(int which, int value, uint8_t *out)
{
    out[0] = 0x18; out[1] = (uint8_t)which; out[2] = 0x20; out[3] = (uint8_t)value;
    return 4;
}

size_t nano_capture_select(int slot, uint8_t *out)
{
    return selector(1, slot, out);
}

size_t nano_cab_select(int slot, uint8_t *out)
{
    return selector(3, slot, out);
}

// FxTypeValue { 3: slot 0-4, 4: model type }.
size_t nano_fx_model_select(int slot, uint32_t type, uint8_t *out)
{
    out[0] = 0x18; out[1] = (uint8_t)slot; out[2] = 0x20;
    return 3 + put_varint(out + 3, type);
}

// FxParameters request { 1: READ, 3: slot 0-4 }.
size_t nano_fx_params_request(int slot, uint8_t *out)
{
    out[0] = 0x08; out[1] = 0x03; out[2] = 0x18; out[3] = (uint8_t)slot;
    return 4;
}

// RetrieveSettings { 1: READ }.
size_t nano_settings_request(uint8_t *out)
{
    out[0] = 0x08; out[1] = 0x03;
    return 2;
}

// UpdateSettings { 14: usbReturnGain (float32, dB) }. The app sends only the setting that changed.
size_t nano_usb_gain(float db, uint8_t *out)
{
    if (db < NANO_USB_GAIN_MIN_DB) db = NANO_USB_GAIN_MIN_DB;
    if (db > 0) db = 0;
    out[0] = 0x75;
    memcpy(out + 1, &db, sizeof(db));
    return 1 + sizeof(db);
}

// RetrieveSettings reply { 17: usbReturnGain (float32, dB) }.
bool nano_settings_usb_gain(const uint8_t *payload, size_t len, float *db)
{
    pb_reader_t r = { payload, payload + len };
    pb_field_t f;
    while (pb_next(&r, &f)) {
        if (f.field != 17) continue;
        if (f.wire == 5) { memcpy(db, f.data, sizeof(float)); return true; }
        if (f.wire == 1) { double d; memcpy(&d, f.data, sizeof(d)); *db = (float)d; return true; }
    }
    return false;
}

// FxParameters reply { 1: 6, 4: values (packed float32, 0-1) }.
int nano_fx_params_values(const uint8_t *payload, size_t len, float *values, int max)
{
    pb_reader_t r = { payload, payload + len };
    pb_field_t f;
    while (pb_next(&r, &f)) {
        if (f.field != 4 || f.wire != 2) continue;
        int count = (int)(f.len / 4) < max ? (int)(f.len / 4) : max;
        for (int i = 0; i < count; i++) memcpy(&values[i], f.data + 4 * i, sizeof(float));
        return count;
    }
    return 0;
}

const nano_fx_model_t *nano_fx_model(uint32_t type)
{
    for (unsigned i = 0; type && i < NANO_FX_MODEL_COUNT; i++) {
        if (NANO_FX_MODELS[i].type == type) return &NANO_FX_MODELS[i];
    }
    return NULL;
}

const char *nano_fx_name(uint32_t type)
{
    const nano_fx_model_t *model = nano_fx_model(type);
    return model ? model->name : type ? "Unknown model" : "Empty";
}
