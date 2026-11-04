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

static const MemoryRegionOps bandit_data_ops = {
    .read = bandit_data_read,
    .write = bandit_data_write,
    .endianness = DEVICE_LITTLE_ENDIAN,
    .valid = {
        .min_access_size = 1,
        .max_access_size = 4,
    },
};

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

    sysbus_init_mmio(sbd, &h->conf_mem);
    sysbus_init_mmio(sbd, &h->data_mem);
    sysbus_init_mmio(sbd, &s->io_window);
    sysbus_init_mmio(sbd, &s->pass_through);
    sysbus_init_mmio(sbd, &s->mem_window);

    name = g_strdup_printf("bandit@%x", s->base);
    h->bus = pci_register_root_bus(dev, name, bandit_set_irq, bandit_map_irq,
                                   s, &s->pci_mmio, &s->pci_io,
                                   PCI_DEVFN(13, 0), BANDIT_NUM_IRQS,
                                   TYPE_PCI_BUS);
    g_free(name);

    self = pci_new(PCI_DEVFN(11, 0), TYPE_BANDIT_PCI_DEVICE);
    qdev_prop_set_uint32(DEVICE(self), "addr-select", s->addr_select);
    pci_realize_and_unref(self, h->bus, errp);
}

static void bandit_host_init(Object *obj)
{
    BanditHostState *s = BANDIT_PCI_HOST_BRIDGE(obj);
    PCIHostState *h = PCI_HOST_BRIDGE(obj);

    memory_region_init_io(&h->conf_mem, obj, &pci_host_conf_le_ops, obj,
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

static void bandit_pci_write_config(PCIDevice *d, uint32_t addr,
                                    uint32_t val, int len)
{
    pci_default_write_config(d, addr, val, len);
    if (ranges_overlap(addr, len, BANDIT_MODE_SELECT, 4) &&
        !(pci_get_long(d->config + BANDIT_MODE_SELECT) & BANDIT_BIG_ENDIAN)) {
        qemu_log_mask(LOG_UNIMP, "bandit: byte-lane reversal selected "
                      "(mode-select 0x%08x)\n",
                      pci_get_long(d->config + BANDIT_MODE_SELECT));
    }
}

static void bandit_pci_reset(DeviceState *dev)
{
    BanditPCIState *s = BANDIT_PCI_DEVICE(dev);
    PCIDevice *d = PCI_DEVICE(dev);

    pci_set_long(d->config + BANDIT_ADDR_SELECT, s->addr_select);
    pci_set_long(d->config + BANDIT_MODE_SELECT, BANDIT_BIG_ENDIAN);
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
