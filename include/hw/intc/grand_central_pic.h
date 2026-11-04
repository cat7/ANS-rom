/*
 * Apple Grand Central interrupt controller
 *
 * SPDX-License-Identifier: GPL-2.0-or-later
 */

#ifndef HW_INTC_GRAND_CENTRAL_PIC_H
#define HW_INTC_GRAND_CENTRAL_PIC_H

#include "hw/core/sysbus.h"
#include "qom/object.h"

#define TYPE_GRAND_CENTRAL_PIC "grand-central-pic"
OBJECT_DECLARE_SIMPLE_TYPE(GrandCentralPICState, GRAND_CENTRAL_PIC)

#define GC_PIC_NUM_IRQS 32

struct GrandCentralPICState {
    SysBusDevice parent_obj;

    MemoryRegion mem;
    qemu_irq irq;

    uint32_t events;
    uint32_t mask;
    uint32_t levels;
    uint32_t latch;
    bool mode1;
};

#endif
