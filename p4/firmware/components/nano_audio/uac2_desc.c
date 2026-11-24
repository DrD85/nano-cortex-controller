// UAC 2.0 descriptor reading (see uac2_desc.h).
#include "uac2_desc.h"

#include <string.h>

#define DESC_INTERFACE 0x04
#define DESC_ENDPOINT 0x05
#define DESC_CS_INTERFACE 0x24
#define CLASS_AUDIO 0x01
#define SUB_CONTROL 0x01
#define SUB_STREAMING 0x02
#define AC_CLOCK_SOURCE 0x0A
#define AS_GENERAL 0x01
#define AS_FORMAT_TYPE 0x02

bool uac2_find_stream(const uint8_t *d, size_t len, uint8_t ep, uac2_stream_t *out)
{
    memset(out, 0, sizeof(*out));
    // what is known about the interface / alternate setting being walked
    uint8_t cur_if = 0, cur_alt = 0, cur_sub = 0, cur_class = 0;
    uint8_t channels = 0, subslot = 0, bits = 0;
    uint8_t ac_if = 0, clock = 0;
    bool found = false;

    for (size_t i = 0; i + 2 <= len;) {
        uint8_t l = d[i], type = d[i + 1];
        if (l < 2 || i + l > len) break;   // broken descriptor: stop, never read past the end
        const uint8_t *x = d + i;
        if (type == DESC_INTERFACE && l >= 9) {
            if (found) break;   // the alternate setting with the endpoint is complete
            cur_if = x[2];
            cur_alt = x[3];
            cur_class = x[5];
            cur_sub = x[6];
            channels = subslot = bits = 0;
            if (cur_class == CLASS_AUDIO && cur_sub == SUB_CONTROL) ac_if = cur_if;
        } else if (type == DESC_CS_INTERFACE && l >= 3 && cur_class == CLASS_AUDIO) {
            uint8_t sub = x[2];
            if (cur_sub == SUB_CONTROL && sub == AC_CLOCK_SOURCE && l >= 4 && !clock) clock = x[3];
            if (cur_sub == SUB_STREAMING && sub == AS_GENERAL && l >= 11) channels = x[10];
            if (cur_sub == SUB_STREAMING && sub == AS_FORMAT_TYPE && l >= 6) {
                subslot = x[4];
                bits = x[5];
            }
        } else if (type == DESC_ENDPOINT && l >= 7 && cur_class == CLASS_AUDIO && cur_sub == SUB_STREAMING) {
            uint8_t addr = x[2], attr = x[3];
            uint16_t mps = (uint16_t)(x[4] | (x[5] << 8)) & 0x7FF;
            uint8_t usage = (attr >> 4) & 3;
            if (addr == ep) {
                found = true;
                out->interface = cur_if;
                out->alt = cur_alt;
                out->ep = addr;
                out->mps = mps;
                out->interval = x[6];
                out->sync = (attr >> 2) & 3;
                out->usage = usage;
                out->channels = channels;
                out->subslot = subslot;
                out->bits = bits;
            } else if (found && usage == 1) {   // explicit feedback endpoint of the same alternate setting
                out->fb_ep = addr;
                out->fb_mps = mps;
                out->fb_interval = x[6];
            }
        }
        i += l;
    }
    out->ac_interface = ac_if;
    out->clock_id = clock;
    return found;
}
