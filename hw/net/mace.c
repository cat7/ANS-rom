/*
 * AMD Am79C940 MACE Ethernet controller (register file only)
 *
 * SPDX-License-Identifier: GPL-2.0-or-later
 *
 * Thirty-two byte registers on 0x10 centres.  No frames are moved: the
 * chip identifies itself, latches its configuration and address
 * filters, and completes a software reset.
 */

#include "qemu/osdep.h"
#include "qemu/log.h"
#include "qemu/module.h"
#include "hw/core/irq.h"
#include "hw/net/mace.h"
#include "migration/vmstate.h"
#include "trace.h"

#define MACE_XMTFS      3
#define MACE_RCVFS      6
#define MACE_FIFOFC     7
#define MACE_IR         8
#define MACE_BIUCC      11
#define MACE_CHIPID0    16
#define MACE_CHIPID1    17
#define MACE_IAC        18
#define MACE_LADRF      20
#define MACE_PADR       21
#define MACE_MPC        24

#define BIUCC_SWRST     0x01
#define IAC_ADDRCHG     0x80
#define IAC_PHYADDR     0x04
#define IAC_LOGADDR     0x02

#define MACE_ID         0x0941

static void mace_reset(DeviceState *dev)
{
    MACEState *s = MACE(dev);

    memset(s->regs, 0, sizeof(s->regs));
    s->regs[MACE_CHIPID0] = MACE_ID & 0xff;
    s->regs[MACE_CHIPID1] = MACE_ID >> 8;
    s->addr_ptr = 0;
}

static uint64_t mace_read(void *opaque, hwaddr addr, unsigned size)
{
    MACEState *s = opaque;
    unsigned reg = (addr >> 4) & 0x1f;
    uint8_t val = s->regs[reg];

    switch (reg) {
    case MACE_IR:
    case MACE_XMTFS:
    case MACE_MPC:
        s->regs[reg] = 0;
        qemu_irq_lower(s->irq);
        break;
    case MACE_LADRF:
        val = s->ladrf[s->addr_ptr & 7];
        break;
    case MACE_PADR:
        val = s->padr[s->addr_ptr % 6];
        break;
    }
    trace_mace_read(reg, val);
    return val;
}

static void mace_write(void *opaque, hwaddr addr, uint64_t val, unsigned size)
{
    MACEState *s = opaque;
    unsigned reg = (addr >> 4) & 0x1f;

    trace_mace_write(reg, val);
    switch (reg) {
    case MACE_BIUCC:
        if (val & BIUCC_SWRST) {
            mace_reset(DEVICE(s));
            val &= ~BIUCC_SWRST;
        }
        s->regs[reg] = val;
        break;
    case MACE_IAC:
        if ((val & IAC_LOGADDR) && (val & IAC_PHYADDR)) {
            val &= ~IAC_PHYADDR;
        }
        if (val & (IAC_LOGADDR | IAC_PHYADDR)) {
            s->addr_ptr = 0;
        }
        s->regs[reg] = val & ~IAC_ADDRCHG;
        break;
    case MACE_LADRF:
        if (s->regs[MACE_IAC] & IAC_LOGADDR) {
            s->ladrf[s->addr_ptr++] = val;
            if (s->addr_ptr >= 8) {
                s->regs[MACE_IAC] &= ~IAC_LOGADDR;
                s->addr_ptr = 0;
            }
        }
        break;
    case MACE_PADR:
        if (s->regs[MACE_IAC] & IAC_PHYADDR) {
            s->padr[s->addr_ptr++] = val;
            if (s->addr_ptr >= 6) {
                s->regs[MACE_IAC] &= ~IAC_PHYADDR;
                s->addr_ptr = 0;
            }
        }
        break;
    case MACE_CHIPID0:
    case MACE_CHIPID1:
    case MACE_IR:
    case MACE_RCVFS:
    case MACE_FIFOFC:
        break;
    default:
        s->regs[reg] = val;
        break;
    }
}

static const MemoryRegionOps mace_ops = {
    .read = mace_read,
    .write = mace_write,
    .endianness = DEVICE_BIG_ENDIAN,
    .impl = {
        .min_access_size = 1,
        .max_access_size = 1,
    },
};

static void mace_init(Object *obj)
{
    MACEState *s = MACE(obj);

    memory_region_init_io(&s->mem, obj, &mace_ops, s, "mace", 0x1000);
    sysbus_init_mmio(SYS_BUS_DEVICE(obj), &s->mem);
    sysbus_init_irq(SYS_BUS_DEVICE(obj), &s->irq);
}

static const VMStateDescription vmstate_mace = {
    .name = "mace",
    .version_id = 1,
    .minimum_version_id = 1,
    .fields = (const VMStateField[]) {
        VMSTATE_UINT8_ARRAY(regs, MACEState, 32),
        VMSTATE_UINT8(addr_ptr, MACEState),
        VMSTATE_UINT8_ARRAY(padr, MACEState, 6),
        VMSTATE_UINT8_ARRAY(ladrf, MACEState, 8),
        VMSTATE_END_OF_LIST()
    }
};

static void mace_class_init(ObjectClass *oc, const void *data)
{
    DeviceClass *dc = DEVICE_CLASS(oc);

    device_class_set_legacy_reset(dc, mace_reset);
    dc->vmsd = &vmstate_mace;
    set_bit(DEVICE_CATEGORY_NETWORK, dc->categories);
}

static const TypeInfo mace_info = {
    .name          = TYPE_MACE,
    .parent        = TYPE_SYS_BUS_DEVICE,
    .instance_size = sizeof(MACEState),
    .instance_init = mace_init,
    .class_init    = mace_class_init,
};

static void mace_register_types(void)
{
    type_register_static(&mace_info);
}

type_init(mace_register_types)
