#pragma once

#include <stdint.h>
#include <stdbool.h>

#define T  250   // usec

struct nexa_payload {
    uint32_t id : 26;
    bool group : 1;
    bool state : 1;
    uint8_t channel : 2;
    uint8_t unit : 2;
};

/** Bit value of OFF state (bit is set) in Nexa frame */
#define NEXA_STATE_OFF 1
/** Bit value of ON state (bit is set) in Nexa frame */
#define NEXA_STATE_ON 0