#pragma once

#define T  250   // usec

struct nexa_payload {
    uint32_t id : 26;
    bool group : 1;
    bool state : 1;
    uint8_t channel : 2;
    uint8_t unit : 2;
};