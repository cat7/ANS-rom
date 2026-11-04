/*
 * Apple Hammerhead (343S1142) system bus, DRAM, ROM and L2 controller
 *
 * Register behaviour after the Granny Smith emulator's model (MIT,
 * Copyright (c) pappadf).
 *
 * SPDX-License-Identifier: GPL-2.0-or-later
 *
 * 128 32-bit registers on 0x10 centres, big-endian; byte registers sit
 * in lane 0 of their centre.  DRAM is decoded through 26 bank base
 * register pairs: bank k has its A word at 0x1C0 + 0x20k and its B word
 * at 0x1D0 + 0x20k.  B[31:24] is the base in 4 MB units, A[24] is base
 * bit 30, A[26] pairs the bank with the next one (interleaved).  A bank
 * shows its DIMM aliased through a window of up to 64 MB at its base;
 * empty banks decode nothing.
 */

#include "qemu/osdep.h"
#include "qapi/error.h"
#include "qemu/log.h"
#include "qemu/module.h"
#include "qemu/units.h"
#include "hw/core/qdev-properties.h"
#include "hw/misc/hammerhead.h"
#include "migration/vmstate.h"
#include "trace.h"

#define HH_ID           0x00
#define HH_MACHID       0x20
#define HH_ARBCONFIG    0x90
#define HH_WHOAMI       0xB0
#define HH_INTREG       0xC0
#define HH_L2CFG        0xE0
#define HH_L2STROBE     0xF0

#define HH_L2CFG_STRAP  0x83

#define HH_BANK_A(k)    (0x1C0 + 0x20 * (k))
#define HH_BANK_B(k)    (0x1D0 + 0x20 * (k))
#define HH_BANK_END     0x500
#define HH_BANK_A_ILV   0x04000000u
#define HH_BANK_WINDOW  (64 * MiB)
#define HH_DECODE_TOP   0x80000000u

#define HH_DIMM_PAIRS   4
#define HH_DIMM_BANK(pair, side) (2 + 4 * (pair) + (side))

static uint8_t hh_l2_strap(HammerheadState *s)
{
    switch (s->l2_kb) {
    case 256:
        return 0x81;
    case 512:
        return 0x80;
    case 1024:
        return 0x82;
    case 4096:
        return 0x83;
    default:
        return 0x00;
    }
}

static uint32_t hh_bank_base(HammerheadState *s, int k)
{
    uint32_t a = s->reg[HH_BANK_A(k) >> 4];
    uint32_t b = s->reg[HH_BANK_B(k) >> 4];

    return ((b >> 24) << 22) | ((a & 0x01000000u) ? 0x40000000u : 0);
}

static void hh_remap(HammerheadState *s)
{
    int k, j, n;

    memory_region_transaction_begin();
    for (k = 0; k < HAMMERHEAD_BANKS; k++) {
        for (n = 0; n < s->nalias[k]; n++) {
            memory_region_del_subregion(&s->dram, &s->alias[k][n]);
            object_unparent(OBJECT(&s->alias[k][n]));
        }
        s->nalias[k] = 0;
    }

    for (k = 0; k < HAMMERHEAD_BANKS; k++) {
        uint32_t size = s->bank_size[k];
        uint32_t base, window;
        bool ilv_lower, ilv_upper;

        ilv_lower = (s->reg[HH_BANK_A(k) >> 4] & HH_BANK_A_ILV) &&
                    k + 1 < HAMMERHEAD_BANKS &&
                    s->bank_size[k + 1] == size &&
                    s->bank_off[k + 1] == s->bank_off[k] + size;
        ilv_upper = k > 0 &&
                    (s->reg[HH_BANK_A(k - 1) >> 4] & HH_BANK_A_ILV) &&
                    s->bank_size[k - 1] == size &&
                    s->bank_off[k] == s->bank_off[k - 1] + size;
        if (ilv_lower) {
            size *= 2;
        }
        if (!size || ilv_upper) {
            continue;
        }
        base = hh_bank_base(s, k);
        if (base >= HH_DECODE_TOP) {
            continue;
        }
        window = ilv_lower ? 2 * HH_BANK_WINDOW : HH_BANK_WINDOW;
        for (j = 0; j < HAMMERHEAD_BANKS; j++) {
            uint32_t other = hh_bank_base(s, j);

            if (j != k && other > base && other - base < window) {
                window = other - base;
            }
        }
        if (base + (uint64_t)window > HH_DECODE_TOP) {
            window = HH_DECODE_TOP - base;
        }
        for (n = 0; n * (uint64_t)size < window && n < HAMMERHEAD_MAX_ALIAS;
             n++) {
            uint32_t len = MIN(size, window - n * size);

            memory_region_init_alias(&s->alias[k][n], OBJECT(s), "hh-bank",
                                     s->ram, s->bank_off[k], len);
            memory_region_add_subregion_overlap(&s->dram, base + n * size,
                                                &s->alias[k][n], k);
        }
        s->nalias[k] = n;
        trace_hammerhead_bank(k, base, size, window);
    }
    memory_region_transaction_commit();
}

static uint64_t hh_read(void *opaque, hwaddr addr, unsigned size)
{
    HammerheadState *s = opaque;
    uint64_t val = 0;
    unsigned i;

    for (i = 0; i < size; i++) {
        hwaddr a = addr + i;
        unsigned lane = a & 0xf;
        uint8_t b = 0;

        if (a < HAMMERHEAD_REGS * 0x10 && lane < 4) {
            b = s->reg[a >> 4] >> (24 - 8 * lane);
        }
        val = (val << 8) | b;
    }
    trace_hammerhead_read(addr, size, val);
    return val;
}

static void hh_write(void *opaque, hwaddr addr, uint64_t val, unsigned size)
{
    HammerheadState *s = opaque;
    bool remap = false;
    unsigned i;

    trace_hammerhead_write(addr, size, val);
    for (i = 0; i < size; i++) {
        hwaddr a = addr + i;
        unsigned lane = a & 0xf;
        uint8_t b = val >> (8 * (size - 1 - i));
        uint32_t *r;

        if (a >= HAMMERHEAD_REGS * 0x10 || lane >= 4) {
            continue;
        }
        if ((a >> 4) == (HH_L2CFG >> 4) && lane == 0) {
            b = (b & ~HH_L2CFG_STRAP) | (hh_l2_strap(s) & HH_L2CFG_STRAP);
        }
        r = &s->reg[a >> 4];
        *r = (*r & ~(0xffu << (24 - 8 * lane))) |
             ((uint32_t)b << (24 - 8 * lane));
        if ((a >> 4) == (HH_ID >> 4)) {
            *r = (*r & 0xffff) | (s->id & 0xffff0000u);
        }
        if (a >= HH_BANK_A(0) && a < HH_BANK_END && lane == 0) {
            remap = true;
        }
    }
    if (remap) {
        hh_remap(s);
    }
}

static const MemoryRegionOps hh_ops = {
    .read = hh_read,
    .write = hh_write,
    .endianness = DEVICE_BIG_ENDIAN,
    .valid = {
        .min_access_size = 1,
        .max_access_size = 4,
    },
    .impl = {
        .min_access_size = 1,
        .max_access_size = 4,
    },
};

/*
 * Carve RAM into matched DIMM pairs: the largest power of two up to
 * 64 MB that fits half the remainder, per slot pair.
 */
static void hh_inventory(HammerheadState *s, Error **errp)
{
    uint64_t rest = memory_region_size(s->ram);
    uint32_t off = 0;
    int pair, side;

    memset(s->bank_size, 0, sizeof(s->bank_size));
    memset(s->bank_off, 0, sizeof(s->bank_off));
    for (pair = 0; pair < HH_DIMM_PAIRS && rest >= 2 * MiB; pair++) {
        uint64_t half = rest / 2;
        uint32_t dimm = HH_BANK_WINDOW;

        while (dimm > half) {
            dimm >>= 1;
        }
        for (side = 0; side < 2; side++) {
            s->bank_size[HH_DIMM_BANK(pair, side)] = dimm;
            s->bank_off[HH_DIMM_BANK(pair, side)] = off;
            off += dimm;
        }
        rest -= 2 * (uint64_t)dimm;
    }
    if (rest) {
        error_setg(errp, "RAM size does not decompose into DIMM pairs "
                   "(0x%" PRIx64 " bytes left over)", rest);
    }
}

static void hh_reset(DeviceState *dev)
{
    HammerheadState *s = HAMMERHEAD(dev);

    memset(s->reg, 0, sizeof(s->reg));
    s->reg[HH_ID >> 4] = s->id;
    s->reg[HH_MACHID >> 4] = s->machid;
    s->reg[HH_WHOAMI >> 4] = 0x10u << 24;
    s->reg[HH_L2CFG >> 4] = (uint32_t)hh_l2_strap(s) << 24;
    hh_remap(s);
}

static void hh_realize(DeviceState *dev, Error **errp)
{
    HammerheadState *s = HAMMERHEAD(dev);

    if (!s->ram) {
        error_setg(errp, "hammerhead: 'ram' link not set");
        return;
    }
    hh_inventory(s, errp);
}

static void hh_init(Object *obj)
{
    HammerheadState *s = HAMMERHEAD(obj);
    SysBusDevice *sbd = SYS_BUS_DEVICE(obj);

    memory_region_init_io(&s->regs, obj, &hh_ops, s, "hammerhead",
                          HAMMERHEAD_REGS * 0x10);
    memory_region_init(&s->dram, obj, "hammerhead-dram", HH_DECODE_TOP);
    sysbus_init_mmio(sbd, &s->regs);
    sysbus_init_mmio(sbd, &s->dram);
}

static int hh_post_load(void *opaque, int version_id)
{
    hh_remap(opaque);
    return 0;
}

static const VMStateDescription vmstate_hammerhead = {
    .name = "hammerhead",
    .version_id = 1,
    .minimum_version_id = 1,
    .post_load = hh_post_load,
    .fields = (const VMStateField[]) {
        VMSTATE_UINT32_ARRAY(reg, HammerheadState, HAMMERHEAD_REGS),
        VMSTATE_END_OF_LIST()
    }
};

static const Property hh_properties[] = {
    DEFINE_PROP_LINK("ram", HammerheadState, ram, TYPE_MEMORY_REGION,
                     MemoryRegion *),
    DEFINE_PROP_UINT32("id", HammerheadState, id, 0x39000000),
    DEFINE_PROP_UINT32("machid", HammerheadState, machid, 0x40000000),
    DEFINE_PROP_UINT32("l2-kb", HammerheadState, l2_kb, 1024),
};

static void hh_class_init(ObjectClass *oc, const void *data)
{
    DeviceClass *dc = DEVICE_CLASS(oc);

    dc->realize = hh_realize;
    device_class_set_legacy_reset(dc, hh_reset);
    dc->vmsd = &vmstate_hammerhead;
    device_class_set_props(dc, hh_properties);
}

static const TypeInfo hh_info = {
    .name          = TYPE_HAMMERHEAD,
    .parent        = TYPE_SYS_BUS_DEVICE,
    .instance_size = sizeof(HammerheadState),
    .instance_init = hh_init,
    .class_init    = hh_class_init,
};

static void hh_register_types(void)
{
    type_register_static(&hh_info);
}

type_init(hh_register_types)
