/*
 * AMD Am79C940 MACE Ethernet controller
 *
 * SPDX-License-Identifier: GPL-2.0-or-later
 */

#ifndef HW_NET_MACE_H
#define HW_NET_MACE_H

#include "hw/core/sysbus.h"
#include "qom/object.h"

#define TYPE_MACE "mace"
OBJECT_DECLARE_SIMPLE_TYPE(MACEState, MACE)

struct MACEState {
    SysBusDevice parent_obj;

    MemoryRegion mem;
    qemu_irq irq;
    uint8_t regs[32];
    uint8_t addr_ptr;
    uint8_t padr[6];
    uint8_t ladrf[8];
};

#endif
