/*
 * Apple Bandit (343S1126) PCI host bridge
 *
 * Register behaviour after the Granny Smith emulator's model (MIT,
 * Copyright (c) pappadf).
 *
 * SPDX-License-Identifier: GPL-2.0-or-later
 *
 * Config address port at base + 0x800000, config data port at
 * base + 0xC00000, both little-endian.  Type 0 cycles select a device
 * one-hot on AD11..AD31; the bridge itself answers at IDSEL 11.  The 8 MB
 * at the bridge base is PCI I/O space (16 address bits driven, so the
 * 64 KB space aliases through it); the next 16 MB passes memory cycles
 * through one to one.
 */

#include "qemu/osdep.h"
#include "qapi/error.h"
#include "qemu/log.h"
#include "qemu/module.h"
#include "qemu/units.h"
#include "hw/core/irq.h"
#include "hw/core/qdev-properties.h"
#include "hw/pci/pci_device.h"
#include "hw/pci-host/bandit.h"
#include "system/address-spaces.h"
#include "trace.h"

#define BANDIT_ADDR_SELECT  0x48
#define BANDIT_MODE_SELECT  0x50
#define BANDIT_BIG_ENDIAN   0x01000000u

static int bandit_map_irq(PCIDevice *pci_dev, int irq_num)
{
    return PCI_SLOT(pci_dev->devfn);
}

static void bandit_set_irq(void *opaque, int irq_num, int level)
{
    BanditHostState *s = opaque;

    qemu_set_irq(s->irqs[irq_num], level);
}

/* -1 when the cycle selects nothing */
static int bandit_cfg_addr(BanditHostState *s, hwaddr port)
{
    uint32_t a = PCI_HOST_BRIDGE(s)->config_reg;
    uint32_t idsel = a & 0xfffff800u;
    int slot;

    if (a & 1) {
        return -1;              /* type 1: no subordinate buses */
    }
    if (idsel == 0 || (idsel & (idsel - 1))) {
        return -1;
    }
    slot = ctz32(idsel);
    return (PCI_DEVFN(slot, (a >> 8) & 7) << 8) | (a & 0xfc) | (port & 3);
}

static uint64_t bandit_data_read(void *opaque, hwaddr addr, unsigned len)
{
    BanditHostState *s = opaque;
    int cfg = bandit_cfg_addr(s, addr);
    uint64_t val;

    if (cfg < 0) {
        val = (1ull << (len * 8)) - 1;
    } else {
        val = pci_data_read(PCI_HOST_BRIDGE(s)->bus, cfg, len);
    }
    trace_bandit_cfg_read(s->base, PCI_HOST_BRIDGE(s)->config_reg, addr,
                          len, val);
    return val;
}

static void bandit_data_write(void *opaque, hwaddr addr, uint64_t val,
                              unsigned len)
{
    BanditHostState *s = opaque;
    int cfg = bandit_cfg_addr(s, addr);

    trace_bandit_cfg_write(s->base, PCI_HOST_BRIDGE(s)->config_reg, addr,
                           len, val);
    if (cfg >= 0) {
        pci_data_write(PCI_HOST_BRIDGE(s)->bus, cfg, val, len);
    }
}

/* the address latch decodes its four bytes on (offset & 3) */
static uint64_t bandit_conf_read(void *opaque, hwaddr addr, unsigned len)
{
    PCIHostState *h = opaque;
    uint64_t val = 0;

    for (unsigned i = 0; i < len; i++) {
        val |= (uint64_t)((h->config_reg >> (8 * ((addr + i) & 3))) & 0xff)
               << (8 * i);
    }
    return val;
}

static void bandit_conf_write(void *opaque, hwaddr addr, uint64_t val,
                              unsigned len)
{
    PCIHostState *h = opaque;

    for (unsigned i = 0; i < len; i++) {
        unsigned sh = 8 * ((addr + i) & 3);

        h->config_reg = (h->config_reg & ~(0xffu << sh)) |
                        (((val >> (8 * i)) & 0xff) << sh);
    }
}

static const MemoryRegionOps bandit_conf_ops = {
    .read = bandit_conf_read,
    .write = bandit_conf_write,
    .endianness = DEVICE_LITTLE_ENDIAN,
    .valid = {
        .min_access_size = 1,
        .max_access_size = 4,
    },
};

static const MemoryRegionOps bandit_data_ops = {
    .read = bandit_data_read,
    .write = bandit_data_write,
    .endianness = DEVICE_LITTLE_ENDIAN,
    .valid = {
        .min_access_size = 1,
        .max_access_size = 4,
    },
};

/*
 * Byte-lane reversal (mode-select ENDIAN bit clear): processor-bus byte n
 * is PCI byte n ^ 7.  With the 604's little-endian address munge the two
 * cancel, so a little-endian CPU sees PCI and DMA see memory as on an
 * address-invariant little-endian bus.
 */
static uint64_t bandit_lane_read(void *opaque, hwaddr addr, unsigned size)
{
    BanditLane *l = opaque;
    MemTxAttrs attrs = MEMTXATTRS_UNSPECIFIED;

    addr ^= 8 - size;
    switch (size) {
    case 1:
        return address_space_ldub(&l->as, addr, attrs, NULL);
    case 2:
        return address_space_lduw_le(&l->as, addr, attrs, NULL);
    case 4:
        return address_space_ldl_le(&l->as, addr, attrs, NULL);
    default:
        return address_space_ldq_le(&l->as, addr, attrs, NULL);
    }
}

static void bandit_lane_write(void *opaque, hwaddr addr, uint64_t val,
                              unsigned size)
{
    BanditLane *l = opaque;
    MemTxAttrs attrs = MEMTXATTRS_UNSPECIFIED;

    addr ^= 8 - size;
    switch (size) {
    case 1:
        address_space_stb(&l->as, addr, val, attrs, NULL);
        break;
    case 2:
        address_space_stw_le(&l->as, addr, val, attrs, NULL);
        break;
    case 4:
        address_space_stl_le(&l->as, addr, val, attrs, NULL);
        break;
    default:
        address_space_stq_le(&l->as, addr, val, attrs, NULL);
        break;
    }
}

static const MemoryRegionOps bandit_lane_ops = {
    .read = bandit_lane_read,
    .write = bandit_lane_write,
    .endianness = DEVICE_BIG_ENDIAN,
    .valid = {
        .min_access_size = 1,
        .max_access_size = 8,
    },
};

/* bus-master accesses to host memory while the lanes are reversed */
static uint64_t bandit_dma_read(void *opaque, hwaddr addr, unsigned size)
{
    MemTxAttrs attrs = MEMTXATTRS_UNSPECIFIED;

    addr ^= 8 - size;
    switch (size) {
    case 1:
        return address_space_ldub(&address_space_memory, addr, attrs, NULL);
    case 2:
        return address_space_lduw_be(&address_space_memory, addr, attrs, NULL);
    case 4:
        return address_space_ldl_be(&address_space_memory, addr, attrs, NULL);
    default:
        return address_space_ldq_be(&address_space_memory, addr, attrs, NULL);
    }
}

static void bandit_dma_write(void *opaque, hwaddr addr, uint64_t val,
                             unsigned size)
{
    MemTxAttrs attrs = MEMTXATTRS_UNSPECIFIED;

    addr ^= 8 - size;
    switch (size) {
    case 1:
        address_space_stb(&address_space_memory, addr, val, attrs, NULL);
        break;
    case 2:
        address_space_stw_be(&address_space_memory, addr, val, attrs, NULL);
        break;
    case 4:
        address_space_stl_be(&address_space_memory, addr, val, attrs, NULL);
        break;
    default:
        address_space_stq_be(&address_space_memory, addr, val, attrs, NULL);
        break;
    }
}

static const MemoryRegionOps bandit_dma_ops = {
    .read = bandit_dma_read,
    .write = bandit_dma_write,
    .endianness = DEVICE_LITTLE_ENDIAN,
    .valid = {
        .min_access_size = 1,
        .max_access_size = 8,
    },
};

static AddressSpace *bandit_dma_as(PCIBus *bus, void *opaque, int devfn)
{
    BanditHostState *s = opaque;

    return &s->dma_as;
}

static const PCIIOMMUOps bandit_iommu_ops = {
    .get_address_space = bandit_dma_as,
};

/* the window the sysbus maps: mr[off, off + size), straight or reversed */
static MemoryRegion *bandit_lane_init(BanditHostState *s, int n,
                                      const char *name, MemoryRegion *mr,
                                      hwaddr off, uint64_t size)
{
    BanditLane *l = &s->lanes[n];
    Object *o = OBJECT(s);

    memory_region_init(&l->outer, o, name, size);
    memory_region_init_alias(&l->straight, o, name, mr, off, size);
    memory_region_add_subregion(&l->outer, 0, &l->straight);
    memory_region_init_io(&l->reversed, o, &bandit_lane_ops, l, name, size);
    /* a forwarder: the access it makes may land on this device again */
    l->reversed.disable_reentrancy_guard = true;
    memory_region_set_enabled(&l->reversed, false);
    memory_region_add_subregion_overlap(&l->outer, 0, &l->reversed, 1);

    memory_region_init(&l->root, o, name, size);
    memory_region_init_alias(&l->target, o, name, mr, off, size);
    memory_region_add_subregion(&l->root, 0, &l->target);
    address_space_init(&l->as, &l->root, name);
    return &l->outer;
}

void bandit_set_lanes_reversed(BanditHostState *s, bool on)
{
    if (s->lanes_reversed == on) {
        return;
    }
    trace_bandit_lanes(s->base, on);
    s->lanes_reversed = on;
    memory_region_transaction_begin();
    for (int i = 0; i < BANDIT_NUM_LANES; i++) {
        memory_region_set_enabled(&s->lanes[i].reversed, on);
    }
    memory_region_set_enabled(&s->dma_reversed, on);
    memory_region_transaction_commit();
}

static void bandit_host_realize(DeviceState *dev, Error **errp)
{
    BanditHostState *s = BANDIT_PCI_HOST_BRIDGE(dev);
    PCIHostState *h = PCI_HOST_BRIDGE(dev);
    SysBusDevice *sbd = SYS_BUS_DEVICE(dev);
    PCIDevice *self;
    char *name;
    int i;

    memory_region_init(&s->pci_mmio, OBJECT(s), "bandit-pci-mmio",
                       0x100000000ULL);
    memory_region_init_io(&s->pci_io, OBJECT(s), &unassigned_io_ops, s,
                          "bandit-pci-io", 0x10000);

    memory_region_init(&s->io_window, OBJECT(s), "bandit-io-window",
                       BANDIT_IO_SIZE);
    for (i = 0; i < ARRAY_SIZE(s->io_alias); i++) {
        memory_region_init_alias(&s->io_alias[i], OBJECT(s), "bandit-io",
                                 &s->pci_io, 0, 0x10000);
        memory_region_add_subregion(&s->io_window, i * 0x10000,
                                    &s->io_alias[i]);
    }
    memory_region_init_alias(&s->pass_through, OBJECT(s),
                             "bandit-pass-through", &s->pci_mmio,
                             s->base + 16 * MiB, 16 * MiB);
    memory_region_init_alias(&s->mem_window, OBJECT(s), "bandit-pci-mem",
                             &s->pci_mmio, s->mem_base, 256 * MiB);

    sysbus_init_mmio(sbd, bandit_lane_init(s, 0, "bandit-pci-conf-idx",
                                           &h->conf_mem, 0, 0x1000));
    sysbus_init_mmio(sbd, bandit_lane_init(s, 1, "bandit-pci-conf-data",
                                           &h->data_mem, 0, 0x1000));
    sysbus_init_mmio(sbd, bandit_lane_init(s, 2, "bandit-io-window",
                                           &s->io_window, 0, BANDIT_IO_SIZE));
    sysbus_init_mmio(sbd, bandit_lane_init(s, 3, "bandit-pass-through",
                                           &s->pci_mmio, s->base + 16 * MiB,
                                           16 * MiB));
    sysbus_init_mmio(sbd, bandit_lane_init(s, 4, "bandit-pci-mem",
                                           &s->pci_mmio, s->mem_base,
                                           256 * MiB));

    memory_region_init(&s->dma_root, OBJECT(s), "bandit-dma", UINT64_MAX);
    memory_region_init_alias(&s->dma_straight, OBJECT(s), "bandit-dma",
                             get_system_memory(), 0, UINT64_MAX);
    memory_region_add_subregion(&s->dma_root, 0, &s->dma_straight);
    memory_region_init_io(&s->dma_reversed, OBJECT(s), &bandit_dma_ops, s,
                          "bandit-dma-reversed", 0x100000000ULL);
    s->dma_reversed.disable_reentrancy_guard = true;
    memory_region_set_enabled(&s->dma_reversed, false);
    memory_region_add_subregion_overlap(&s->dma_root, 0, &s->dma_reversed, 1);
    address_space_init(&s->dma_as, &s->dma_root, "bandit-dma");

    name = g_strdup_printf("bandit@%x", s->base);
    h->bus = pci_register_root_bus(dev, name, bandit_set_irq, bandit_map_irq,
                                   s, &s->pci_mmio, &s->pci_io,
                                   PCI_DEVFN(13, 0), BANDIT_NUM_IRQS,
                                   TYPE_PCI_BUS);
    g_free(name);
    pci_setup_iommu(h->bus, &bandit_iommu_ops, s);

    self = pci_new(PCI_DEVFN(11, 0), TYPE_BANDIT_PCI_DEVICE);
    qdev_prop_set_uint32(DEVICE(self), "addr-select", s->addr_select);
    pci_realize_and_unref(self, h->bus, errp);
}

static void bandit_host_init(Object *obj)
{
    BanditHostState *s = BANDIT_PCI_HOST_BRIDGE(obj);
    PCIHostState *h = PCI_HOST_BRIDGE(obj);

    memory_region_init_io(&h->conf_mem, obj, &bandit_conf_ops, obj,
                          "bandit-pci-conf-idx", 0x1000);
    memory_region_init_io(&h->data_mem, obj, &bandit_data_ops, obj,
                          "bandit-pci-conf-data", 0x1000);
    qdev_init_gpio_out(DEVICE(obj), s->irqs, ARRAY_SIZE(s->irqs));
}

static char *bandit_ofw_unit_address(const SysBusDevice *dev)
{
    BanditHostState *s = BANDIT_PCI_HOST_BRIDGE(dev);

    return g_strdup_printf("%x", s->base);
}

static const Property bandit_host_properties[] = {
    DEFINE_PROP_UINT32("base", BanditHostState, base, 0xf2000000),
    DEFINE_PROP_UINT32("mem-base", BanditHostState, mem_base, 0x80000000),
    DEFINE_PROP_UINT32("addr-select", BanditHostState, addr_select,
                       0x0100000c),
};

static void bandit_host_class_init(ObjectClass *klass, const void *data)
{
    DeviceClass *dc = DEVICE_CLASS(klass);
    SysBusDeviceClass *sbc = SYS_BUS_DEVICE_CLASS(klass);

    dc->realize = bandit_host_realize;
    device_class_set_props(dc, bandit_host_properties);
    sbc->explicit_ofw_unit_address = bandit_ofw_unit_address;
    set_bit(DEVICE_CATEGORY_BRIDGE, dc->categories);
    dc->fw_name = "pci";
}

/* The bridge's own header at IDSEL 11 */

typedef struct BanditPCIState {
    PCIDevice parent_obj;
    uint32_t addr_select;
} BanditPCIState;

OBJECT_DECLARE_SIMPLE_TYPE(BanditPCIState, BANDIT_PCI_DEVICE)

static BanditHostState *bandit_host_of(PCIDevice *d)
{
    return BANDIT_PCI_HOST_BRIDGE(BUS(pci_get_bus(d))->parent);
}

static void bandit_pci_write_config(PCIDevice *d, uint32_t addr,
                                    uint32_t val, int len)
{
    pci_default_write_config(d, addr, val, len);
    if (ranges_overlap(addr, len, BANDIT_MODE_SELECT, 4)) {
        bandit_set_lanes_reversed(bandit_host_of(d),
            !(pci_get_long(d->config + BANDIT_MODE_SELECT) &
              BANDIT_BIG_ENDIAN));
    }
}

static void bandit_pci_reset(DeviceState *dev)
{
    BanditPCIState *s = BANDIT_PCI_DEVICE(dev);
    PCIDevice *d = PCI_DEVICE(dev);

    pci_set_long(d->config + BANDIT_ADDR_SELECT, s->addr_select);
    pci_set_long(d->config + BANDIT_MODE_SELECT, BANDIT_BIG_ENDIAN);
    bandit_set_lanes_reversed(bandit_host_of(d), false);
}

static void bandit_pci_realize(PCIDevice *d, Error **errp)
{
    pci_set_long(d->config + PCI_SUBSYSTEM_VENDOR_ID, 0);
    pci_set_long(d->wmask + BANDIT_ADDR_SELECT, 0xffffffff);
    pci_set_long(d->wmask + BANDIT_MODE_SELECT, 0xffffffff);
    bandit_pci_reset(DEVICE(d));
}

static const Property bandit_pci_properties[] = {
    DEFINE_PROP_UINT32("addr-select", BanditPCIState, addr_select, 0),
};

static void bandit_pci_class_init(ObjectClass *klass, const void *data)
{
    PCIDeviceClass *k = PCI_DEVICE_CLASS(klass);
    DeviceClass *dc = DEVICE_CLASS(klass);

    k->realize = bandit_pci_realize;
    k->config_write = bandit_pci_write_config;
    k->vendor_id = PCI_VENDOR_ID_APPLE;
    k->device_id = 0x0001;
    k->revision = 0x03;
    k->class_id = PCI_CLASS_BRIDGE_HOST;
    device_class_set_legacy_reset(dc, bandit_pci_reset);
    device_class_set_props(dc, bandit_pci_properties);
    /* PCI host bridge's own header, not user-creatable */
    dc->user_creatable = false;
}

static const TypeInfo bandit_types[] = {
    {
        .name          = TYPE_BANDIT_PCI_HOST_BRIDGE,
        .parent        = TYPE_PCI_HOST_BRIDGE,
        .instance_size = sizeof(BanditHostState),
        .instance_init = bandit_host_init,
        .class_init    = bandit_host_class_init,
    },
    {
        .name          = TYPE_BANDIT_PCI_DEVICE,
        .parent        = TYPE_PCI_DEVICE,
        .instance_size = sizeof(BanditPCIState),
        .class_init    = bandit_pci_class_init,
        .interfaces = (const InterfaceInfo[]) {
            { INTERFACE_CONVENTIONAL_PCI_DEVICE },
            { },
        },
    },
};

DEFINE_TYPES(bandit_types)
