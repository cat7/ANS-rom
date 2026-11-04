/*
 * Apple Hammerhead memory controller (TNT / Network Server)
 *
 * SPDX-License-Identifier: GPL-2.0-or-later
 */

#ifndef HW_MISC_HAMMERHEAD_H
#define HW_MISC_HAMMERHEAD_H

#include "hw/core/sysbus.h"
#include "qom/object.h"

#define TYPE_HAMMERHEAD "hammerhead"
OBJECT_DECLARE_SIMPLE_TYPE(HammerheadState, HAMMERHEAD)

#define HAMMERHEAD_REGS       128
#define HAMMERHEAD_BANKS      26
#define HAMMERHEAD_MAX_ALIAS  32

struct HammerheadState {
    SysBusDevice parent_obj;

    MemoryRegion regs;
    MemoryRegion dram;          /* 2 GB decode container, mapped at 0 */
    MemoryRegion *ram;          /* the DIMM storage */
    MemoryRegion alias[HAMMERHEAD_BANKS][HAMMERHEAD_MAX_ALIAS];
    uint8_t nalias[HAMMERHEAD_BANKS];

    uint32_t reg[HAMMERHEAD_REGS];
    uint32_t bank_size[HAMMERHEAD_BANKS];
    uint32_t bank_off[HAMMERHEAD_BANKS];

    uint32_t id;
    uint32_t machid;
    uint32_t l2_kb;
};

#endif
