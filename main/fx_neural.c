// Published version: no presets beyond the starting points and players' sounds of fx_info.c (the own build uses
// main/private/fx_neural_private.c, see tools/gen_neural.py).
#include "fx_neural.h"
#include <stddef.h>

const nano_info_sound_t *nano_fx_neural(uint32_t type, int *count)
{
    (void)type;
    *count = 0;
    return NULL;
}
