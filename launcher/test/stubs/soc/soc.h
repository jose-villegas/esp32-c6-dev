#pragma once
#include <stdint.h>

/* No host has these peripheral registers, and no host build ever reaches a
 * read or write of one: the only call sites sit inside DEVICE_BUILD counter
 * tests that the host attribution probe links but never selects. Discarding
 * the access is what keeps a device address off a host pointer. */
#define REG_WRITE(addr, val) ((void)(addr), (void)(val))
#define REG_READ(addr)       ((void)(addr), (uint32_t)0)
