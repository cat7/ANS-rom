/*
 * Apple Grand Central (343S1125) interrupt controller
 *
 * Register behaviour after the Granny Smith emulator's model (MIT,
 * Copyright (c) pappadf).
 *
 * SPDX-License-Identifier: GPL-2.0-or-later
 *
 * Four little-endian registers at island +0x20: Events (+0x0), Mask
 * (+0x4), Clear (+0x8) and Levels (+0xC).  Events latch source rising
 * edges.  In the power-on clear mode the CPU line follows
 * (events | levels) & mask.  A Clear write with bit 31 set selects the
 * second mode, where each enabled source change sets a latch bit, the
 * acknowledge drops them all, and the line follows latch & mask.
 * DBDMA channels (sources 0-10) hold their request until acknowledged.
 */

#include "qemu/osdep.h"
#include "qemu/log.h"
#include "qemu/module.h"
#include "hw/core/irq.h"
#include "hw/intc/grand_central_pic.h"
#include "migration/vmstate.h"
#include "trace.h"

#define GC_EVENTS   0x0
#define GC_MASK     0x4
#define GC_CLEAR    0x8
#define GC_LEVELS   0xc

#define GC_MODE_ACK 0x80000000u
#define GC_DMA_SRCS 0x000007ffu

static void gc_pic_update(GrandCentralPICState *s)
{
    bool line;

    if (s->mode1) {
        line = (s->latch & s->mask) != 0;
    } else {
        line = ((s->events | s->levels) & s->mask) != 0;
    }
    qemu_set_irq(s->irq, line);
}

static void gc_pic_set_irq(void *opaque, int n, int level)
{
    GrandCentralPICState *s = opaque;
    uint32_t bit = 1u << n;
    bool was = (s->levels & bit) != 0;

    trace_grand_central_pic_set_irq(n, level);
    if (level) {
        s->levels |= bit;
        if (!was) {
            s->events |= bit;
            if (s->mask & bit) {
                s->latch |= bit;
            }
        }
    } else {
        s->levels &= ~bit;
        if (was && (s->mask & bit)) {
            s->latch |= bit;
        }
    }
    gc_pic_update(s);
}

static uint64_t gc_pic_read(void *opaque, hwaddr addr, unsigned size)
{
    GrandCentralPICState *s = opaque;
    uint32_t val = 0;

    switch (addr & 0xc) {
    case GC_EVENTS:
        val = s->events;
        break;
    case GC_MASK:
        val = s->mask;
        break;
    case GC_LEVELS:
        val = s->levels;
        break;
    }
    trace_grand_central_pic_read(addr, val);
    return val;
}

static void gc_pic_write(void *opaque, hwaddr addr, uint64_t val,
                         unsigned size)
{
    GrandCentralPICState *s = opaque;
    uint32_t newly;

    trace_grand_central_pic_write(addr, val);
    switch (addr & 0xc) {
    case GC_EVENTS:
    case GC_CLEAR:
        s->events &= ~(val & ~GC_MODE_ACK);
        if ((addr & 0xc) == GC_CLEAR) {
            s->mode1 = (val & GC_MODE_ACK) != 0;
            if (s->mode1) {
                s->latch = 0;
            }
        }
        /* a DBDMA request is held until acknowledged here */
        s->levels &= ~(val & GC_DMA_SRCS);
        break;
    case GC_MASK:
        newly = val & ~s->mask;
        s->mask = val;
        s->latch |= (s->events | s->levels) & newly;
        break;
    case GC_LEVELS:
        qemu_log_mask(LOG_GUEST_ERROR,
                      "grand-central-pic: write to read-only Levels\n");
        break;
    }
    gc_pic_update(s);
}

static const MemoryRegionOps gc_pic_ops = {
    .read = gc_pic_read,
    .write = gc_pic_write,
    .endianness = DEVICE_LITTLE_ENDIAN,
    .impl = {
        .min_access_size = 4,
        .max_access_size = 4,
    },
};

static void gc_pic_reset(DeviceState *d)
{
    GrandCentralPICState *s = GRAND_CENTRAL_PIC(d);

    s->events = 0;
    s->mask = 0;
    s->levels = 0;
    s->latch = 0;
    s->mode1 = false;
    gc_pic_update(s);
}

static void gc_pic_init(Object *obj)
{
    GrandCentralPICState *s = GRAND_CENTRAL_PIC(obj);
    SysBusDevice *sbd = SYS_BUS_DEVICE(obj);

    memory_region_init_io(&s->mem, obj, &gc_pic_ops, s,
                          "grand-central-pic", 0x10);
    sysbus_init_mmio(sbd, &s->mem);
    qdev_init_gpio_in(DEVICE(obj), gc_pic_set_irq, GC_PIC_NUM_IRQS);
    sysbus_init_irq(sbd, &s->irq);
}

static const VMStateDescription vmstate_gc_pic = {
    .name = "grand-central-pic",
    .version_id = 1,
    .minimum_version_id = 1,
    .fields = (const VMStateField[]) {
        VMSTATE_UINT32(events, GrandCentralPICState),
        VMSTATE_UINT32(mask, GrandCentralPICState),
        VMSTATE_UINT32(levels, GrandCentralPICState),
        VMSTATE_UINT32(latch, GrandCentralPICState),
        VMSTATE_BOOL(mode1, GrandCentralPICState),
        VMSTATE_END_OF_LIST()
    }
};

static void gc_pic_class_init(ObjectClass *oc, const void *data)
{
    DeviceClass *dc = DEVICE_CLASS(oc);

    device_class_set_legacy_reset(dc, gc_pic_reset);
    dc->vmsd = &vmstate_gc_pic;
    set_bit(DEVICE_CATEGORY_BRIDGE, dc->categories);
}

static const TypeInfo gc_pic_info = {
    .name          = TYPE_GRAND_CENTRAL_PIC,
    .parent        = TYPE_SYS_BUS_DEVICE,
    .instance_size = sizeof(GrandCentralPICState),
    .instance_init = gc_pic_init,
    .class_init    = gc_pic_class_init,
};

static void gc_pic_register_types(void)
{
    type_register_static(&gc_pic_info);
}

type_init(gc_pic_register_types)
