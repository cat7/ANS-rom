/*
 * PowerMac MacIO device emulation
 *
 * Copyright (c) 2005-2007 Fabrice Bellard
 * Copyright (c) 2007 Jocelyn Mayer
 *
 * Permission is hereby granted, free of charge, to any person obtaining a copy
 * of this software and associated documentation files (the "Software"), to deal
 * in the Software without restriction, including without limitation the rights
 * to use, copy, modify, merge, publish, distribute, sublicense, and/or sell
 * copies of the Software, and to permit persons to whom the Software is
 * furnished to do so, subject to the following conditions:
 *
 * The above copyright notice and this permission notice shall be included in
 * all copies or substantial portions of the Software.
 *
 * THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
 * IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
 * FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL
 * THE AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
 * LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM,
 * OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN
 * THE SOFTWARE.
 */

#include "qemu/osdep.h"
#include "qapi/error.h"
#include "qemu/module.h"
#include "hw/misc/macio/cuda.h"
#include "hw/pci/pci.h"
#include "net/net.h"
#include "hw/ppc/mac_dbdma.h"
#include "hw/core/qdev-properties.h"
#include "migration/vmstate.h"
#include "hw/char/escc.h"
#include "hw/misc/macio/macio.h"
#include "hw/intc/heathrow_pic.h"
#include "qemu/log.h"
#include "qemu/main-loop.h"
#include "system/dma.h"
#include "trace.h"

#define ESCC_CLOCK 3686400

/* Note: this code is strongly inspired by the corresponding code in PearPC */

/*
 * The mac-io has two interfaces to the ESCC. One is called "escc-legacy",
 * while the other one is the normal, current ESCC interface.
 *
 * The magic below creates memory aliases to spawn the escc-legacy device
 * purely by rerouting the respective registers to our escc region. This
 * works because the only difference between the two memory regions is the
 * register layout, not their semantics.
 *
 * Reference: ftp://ftp.software.ibm.com/rs6000/technology/spec/chrp/inwork/CHRP_IORef_1.0.pdf
 */
static void macio_escc_legacy_setup(MacIOState *s)
{
    SysBusDevice *sbd = SYS_BUS_DEVICE(&s->escc);
    MemoryRegion *escc_legacy = g_new(MemoryRegion, 1);
    int i;
    static const int maps[] = {
        0x00, 0x00, /* Command B */
        0x02, 0x20, /* Command A */
        0x04, 0x10, /* Data B */
        0x06, 0x30, /* Data A */
        0x08, 0x40, /* Enhancement B */
        0x0A, 0x50, /* Enhancement A */
        0x80, 0x80, /* Recovery count */
        0x90, 0x90, /* Start A */
        0xa0, 0xa0, /* Start B */
        0xb0, 0xb0, /* Detect AB */
    };

    memory_region_init(escc_legacy, OBJECT(s), "escc-legacy", 256);
    for (i = 0; i < ARRAY_SIZE(maps); i += 2) {
        MemoryRegion *port = g_new(MemoryRegion, 1);
        memory_region_init_alias(port, OBJECT(s), "escc-legacy-port",
                                 sysbus_mmio_get_region(sbd, 0),
                                 maps[i + 1], 0x2);
        memory_region_add_subregion(escc_legacy, maps[i], port);
    }

    memory_region_add_subregion(&s->bar, 0x12000, escc_legacy);
}

static void macio_bar_setup(MacIOState *s)
{
    SysBusDevice *sbd = SYS_BUS_DEVICE(&s->escc);
    MemoryRegion *bar = sysbus_mmio_get_region(sbd, 0);

    memory_region_add_subregion(&s->bar, 0x13000, bar);
    macio_escc_legacy_setup(s);
}

static bool macio_common_children(PCIDevice *d, Error **errp)
{
    MacIOState *s = MACIO(d);
    SysBusDevice *sbd;

    if (!qdev_realize(DEVICE(&s->dbdma), BUS(&s->macio_bus), errp)) {
        return false;
    }
    sbd = SYS_BUS_DEVICE(&s->dbdma);
    memory_region_add_subregion(&s->bar, 0x08000,
                                sysbus_mmio_get_region(sbd, 0));

    qdev_prop_set_uint32(DEVICE(&s->escc), "disabled", 0);
    qdev_prop_set_uint32(DEVICE(&s->escc), "frequency", ESCC_CLOCK);
    qdev_prop_set_uint32(DEVICE(&s->escc), "it_shift", 4);
    qdev_prop_set_uint32(DEVICE(&s->escc), "chnBtype", escc_serial);
    qdev_prop_set_uint32(DEVICE(&s->escc), "chnAtype", escc_serial);
    if (!qdev_realize(DEVICE(&s->escc), BUS(&s->macio_bus), errp)) {
        return false;
    }

    macio_bar_setup(s);

    return true;
}

static bool macio_common_realize(PCIDevice *d, Error **errp)
{
    MacIOState *s = MACIO(d);

    if (!macio_common_children(d, errp)) {
        return false;
    }
    pci_register_bar(d, 0, PCI_BASE_ADDRESS_SPACE_MEMORY, &s->bar);

    return true;
}

static bool macio_realize_ide(MacIOState *s, MACIOIDEState *ide,
                              qemu_irq irq0, qemu_irq irq1, int dmaid,
                              Error **errp)
{
    SysBusDevice *sbd = SYS_BUS_DEVICE(ide);

    qdev_prop_set_uint32(DEVICE(ide), "channel", dmaid);
    object_property_set_link(OBJECT(ide), "dbdma", OBJECT(&s->dbdma),
                             &error_abort);
    macio_ide_register_dma(ide);
    if (!qdev_realize(DEVICE(ide), BUS(&s->macio_bus), errp)) {
        return false;
    }
    sysbus_connect_irq(sbd, 0, irq0);
    sysbus_connect_irq(sbd, 1, irq1);

    return true;
}

static void macio_oldworld_realize(PCIDevice *d, Error **errp)
{
    MacIOState *s = MACIO(d);
    OldWorldMacIOState *os = OLDWORLD_MACIO(d);
    DeviceState *pic_dev = DEVICE(&os->pic);
    SysBusDevice *sbd;

    if (!macio_common_realize(d, errp)) {
        return;
    }

    /*
     * Real Beige G3 hardware's mac-io/heathrow chip reports revision-id=1
     * in its PCI config space (confirmed against a real hardware Open
     * Firmware device-tree dump, SourceFiles/G3/PowerMacG3-device-tree.txt,
     * "revision-id" property under Node mac-io) -- we previously left this
     * at the generic zero default.
     */
    pci_set_byte(&d->config[PCI_REVISION_ID], 1);

    /* Heathrow PIC */
    if (!qdev_realize(DEVICE(&os->pic), BUS(&s->macio_bus), errp)) {
        return;
    }
    sbd = SYS_BUS_DEVICE(&os->pic);
    memory_region_add_subregion(&s->bar, 0x0,
                                sysbus_mmio_get_region(sbd, 0));

    qdev_prop_set_uint64(DEVICE(&s->cuda), "timebase-frequency",
                         s->frequency);
    if (!qdev_realize(DEVICE(&s->cuda), BUS(&s->macio_bus), errp)) {
        return;
    }
    sbd = SYS_BUS_DEVICE(&s->cuda);
    memory_region_add_subregion(&s->bar, 0x16000,
                                sysbus_mmio_get_region(sbd, 0));
    sysbus_connect_irq(sbd, 0, qdev_get_gpio_in(pic_dev, OLDWORLD_CUDA_IRQ));

    sbd = SYS_BUS_DEVICE(&s->escc);
    sysbus_connect_irq(sbd, 0, qdev_get_gpio_in(pic_dev, OLDWORLD_ESCCB_IRQ));
    sysbus_connect_irq(sbd, 1, qdev_get_gpio_in(pic_dev, OLDWORLD_ESCCA_IRQ));
    /*
     * DMA channels for the built-in modem (ch-a) and printer (ch-b)
     * serial ports -- see escc_register_dma()'s comment in escc.c for
     * why this exists (real hardware has these channels; nothing
     * registered them before, so a guest arming one spun in
     * mac_dbdma.c's generic unassigned-channel fallback instead).
     * Pin order matches escc_init1()'s two sysbus_init_irq() loops:
     * chn[0](B).dma_tx_irq, chn[0](B).dma_rx_irq, chn[1](A).dma_tx_irq,
     * chn[1](A).dma_rx_irq -- i.e. pins 2-5, after the two status IRQs.
     */
    sysbus_connect_irq(sbd, 2,
                       qdev_get_gpio_in(pic_dev, OLDWORLD_ESCCB_TX_DMA_IRQ));
    sysbus_connect_irq(sbd, 3,
                       qdev_get_gpio_in(pic_dev, OLDWORLD_ESCCB_RX_DMA_IRQ));
    sysbus_connect_irq(sbd, 4,
                       qdev_get_gpio_in(pic_dev, OLDWORLD_ESCCA_TX_DMA_IRQ));
    sysbus_connect_irq(sbd, 5,
                       qdev_get_gpio_in(pic_dev, OLDWORLD_ESCCA_RX_DMA_IRQ));
    escc_register_dma(&s->escc, &s->dbdma);

    if (!qdev_realize(DEVICE(&os->nvram), BUS(&s->macio_bus), errp)) {
        return;
    }
    sbd = SYS_BUS_DEVICE(&os->nvram);
    memory_region_add_subregion(&s->bar, 0x60000,
                                sysbus_mmio_get_region(sbd, 0));
    pmac_format_nvram_partition_oldworld(&os->nvram);

    /*
     * IDE buses. Channel numbers 0xb/0xc (not the seemingly more
     * "familiar" 0x16/0x18) -- confirmed empirically after fixing
     * DBDMA_CHANNEL_SHIFT (include/hw/ppc/mac_dbdma.h): with the
     * corrected shift, a real ROM's IDE0 DMA-start command was
     * observed live setting RUN on internal channel 0x0b, not 0x16.
     * The previous 0x16/0x18 values were, like AWACS_DMA_IN_CHANNEL
     * before this same fix, tuned to double the real channel number
     * to compensate for the old, wrong shift -- not real hardware
     * values in their own right.
     */
    if (!macio_realize_ide(s, &os->ide[0],
                           qdev_get_gpio_in(pic_dev, OLDWORLD_IDE0_IRQ),
                           qdev_get_gpio_in(pic_dev, OLDWORLD_IDE0_DMA_IRQ),
                           0x0b, errp)) {
        return;
    }

    if (!macio_realize_ide(s, &os->ide[1],
                           qdev_get_gpio_in(pic_dev, OLDWORLD_IDE1_IRQ),
                           qdev_get_gpio_in(pic_dev, OLDWORLD_IDE1_DMA_IRQ),
                           0x0c, errp)) {
        return;
    }

    /*
     * BMAC ethernet: on real hardware, always physically present, so this
     * is unconditional by default. But unlike a pluggable PCI NIC, there's
     * no way to remove it after the fact (e.g. via `-device` vs `-nodefaults`)
     * -- it's baked into this machine model. That makes it impossible to
     * boot a CD/disk whose System Folder has a network extension that
     * crashes when it sees an Ethernet controller (e.g. a buggy AppleTalk
     * library) unless bmac can be left out entirely. Gate it on whether
     * the user actually asked for networking at all (`-nic ...`/`-net ...`,
     * as opposed to `-net none` or no networking option at all) -- if not,
     * skip creating the device altogether rather than just leaving it
     * disconnected from a netdev, so the guest's Open Firmware/Device
     * Manager doesn't see an Ethernet controller in the address space at
     * all.
     */
    if (qemu_find_nic_info(TYPE_BMAC, true, "bmac")) {
        qemu_configure_nic_device(DEVICE(&os->bmac), true, "bmac");
        if (!qdev_realize(DEVICE(&os->bmac), BUS(&s->macio_bus), errp)) {
            return;
        }
        sbd = SYS_BUS_DEVICE(&os->bmac);
        memory_region_add_subregion(&s->bar, 0x11000,
                                    sysbus_mmio_get_region(sbd, 0));
        sysbus_connect_irq(sbd, 0, qdev_get_gpio_in(pic_dev, OLDWORLD_BMAC_IRQ));
        sysbus_connect_irq(sbd, 1,
                           qdev_get_gpio_in(pic_dev, OLDWORLD_BMAC_TX_IRQ));
        sysbus_connect_irq(sbd, 2,
                           qdev_get_gpio_in(pic_dev, OLDWORLD_BMAC_RX_IRQ));
        bmac_register_dma(&os->bmac, &s->dbdma);
    }

    /* MESH SCSI controller: on real hardware, always physically present */
    if (!qdev_realize(DEVICE(&os->mesh), BUS(&s->macio_bus), errp)) {
        return;
    }
    sbd = SYS_BUS_DEVICE(&os->mesh);
    memory_region_add_subregion(&s->bar, 0x10000,
                                sysbus_mmio_get_region(sbd, 0));
    sysbus_connect_irq(sbd, 0, qdev_get_gpio_in(pic_dev, OLDWORLD_MESH_IRQ));
    sysbus_connect_irq(sbd, 1, qdev_get_gpio_in(pic_dev, OLDWORLD_MESH_DMA_IRQ));
    mesh_register_dma(&os->mesh, &s->dbdma);

    /*
     * AWACS sound codec: on real hardware, always physically present.
     * Real Beige G3 ROM plays a startup chime very early in boot and
     * hangs indefinitely polling Codec Status if nothing answers here.
     */
    if (!qdev_realize(DEVICE(&os->awacs), BUS(&s->macio_bus), errp)) {
        return;
    }
    sbd = SYS_BUS_DEVICE(&os->awacs);
    memory_region_add_subregion(&s->bar, 0x14000,
                                sysbus_mmio_get_region(sbd, 0));
    sysbus_connect_irq(sbd, 0, qdev_get_gpio_in(pic_dev, OLDWORLD_AWACS_IRQ));
    sysbus_connect_irq(sbd, 1, qdev_get_gpio_in(pic_dev, OLDWORLD_AWACS_DMA_IRQ));
    awacs_register_dma(&os->awacs, &s->dbdma);

    /*
     * SWIM3 floppy controller: always physically present on the real
     * board. Real register-level SWIM3 model (16 byte-wide registers at
     * a 16-byte stride, 0x1000 window, matching the real device tree's
     * reg size); sector data moves through DBDMA channel 1, main IRQ
     * 0x13, floppy DMA IRQ 0x01.
     */
    if (!qdev_realize(DEVICE(&os->swim), BUS(&s->macio_bus), errp)) {
        return;
    }
    sbd = SYS_BUS_DEVICE(&os->swim);
    memory_region_add_subregion(&s->bar, 0x15000,
                                sysbus_mmio_get_region(sbd, 0));
    sysbus_connect_irq(sbd, 0, qdev_get_gpio_in(pic_dev, OLDWORLD_SWIM_IRQ));
    sysbus_connect_irq(sbd, 1, qdev_get_gpio_in(pic_dev, 0x01));
    swim3_register_dma(&os->swim, &s->dbdma);
}

static void macio_init_ide(MacIOState *s, MACIOIDEState *ide, int index)
{
    gchar *name = g_strdup_printf("ide[%i]", index);
    uint32_t addr = 0x1f000 + ((index + 1) * 0x1000);

    object_initialize_child(OBJECT(s), name, ide, TYPE_MACIO_IDE);
    qdev_prop_set_uint32(DEVICE(ide), "addr", addr);
    memory_region_add_subregion(&s->bar, addr, &ide->mem);
    g_free(name);
}

static void macio_oldworld_init(Object *obj)
{
    MacIOState *s = MACIO(obj);
    OldWorldMacIOState *os = OLDWORLD_MACIO(obj);
    DeviceState *dev;
    int i;

    object_initialize_child(obj, "pic", &os->pic, TYPE_HEATHROW);

    object_initialize_child(obj, "cuda", &s->cuda, TYPE_CUDA);

    object_initialize_child(obj, "nvram", &os->nvram, TYPE_MACIO_NVRAM);
    dev = DEVICE(&os->nvram);
    qdev_prop_set_uint32(dev, "size", MACIO_NVRAM_SIZE);
    qdev_prop_set_uint32(dev, "it_shift", 4);

    for (i = 0; i < 2; i++) {
        macio_init_ide(s, &os->ide[i], i);
    }

    /*
     * Only create the bmac child object at all if the user actually asked
     * for networking (see the realize-time check for why) -- QEMU asserts
     * every object_initialize_child()'d device gets realized before
     * machine init completes, so it's not enough to just skip realizing
     * it later while still creating it here.
     */
    if (qemu_find_nic_info(TYPE_BMAC, true, "bmac")) {
        object_initialize_child(obj, "bmac", &os->bmac, TYPE_BMAC);
    }
    object_initialize_child(obj, "mesh", &os->mesh, TYPE_MESH);
    object_initialize_child(obj, "awacs", &os->awacs, TYPE_AWACS);
    object_initialize_child(obj, "swim", &os->swim, TYPE_SWIM3);
}

static void timer_write(void *opaque, hwaddr addr, uint64_t value,
                       unsigned size)
{
    trace_macio_timer_write(addr, size, value);
}

static uint64_t timer_read(void *opaque, hwaddr addr, unsigned size)
{
    uint32_t value = 0;
    uint64_t systime = qemu_clock_get_ns(QEMU_CLOCK_VIRTUAL);
    uint64_t kltime;

    kltime = muldiv64(systime, 4194300, NANOSECONDS_PER_SECOND * 4);
    kltime = muldiv64(kltime, 18432000, 1048575);

    switch (addr) {
    case 0x38:
        value = kltime;
        break;
    case 0x3c:
        value = kltime >> 32;
        break;
    }

    trace_macio_timer_read(addr, size, value);
    return value;
}

static const MemoryRegionOps timer_ops = {
    .read = timer_read,
    .write = timer_write,
    .endianness = DEVICE_LITTLE_ENDIAN,
};

static void macio_newworld_realize(PCIDevice *d, Error **errp)
{
    MacIOState *s = MACIO(d);
    NewWorldMacIOState *ns = NEWWORLD_MACIO(d);
    DeviceState *pic_dev = DEVICE(&ns->pic);
    SysBusDevice *sbd;
    MemoryRegion *timer_memory = NULL;

    if (!macio_common_realize(d, errp)) {
        return;
    }

    /* OpenPIC */
    qdev_prop_set_uint32(pic_dev, "model", OPENPIC_MODEL_KEYLARGO);
    sbd = SYS_BUS_DEVICE(&ns->pic);
    sysbus_realize_and_unref(sbd, &error_fatal);
    memory_region_add_subregion(&s->bar, 0x40000,
                                sysbus_mmio_get_region(sbd, 0));

    sbd = SYS_BUS_DEVICE(&s->escc);
    sysbus_connect_irq(sbd, 0, qdev_get_gpio_in(pic_dev, NEWWORLD_ESCCB_IRQ));
    sysbus_connect_irq(sbd, 1, qdev_get_gpio_in(pic_dev, NEWWORLD_ESCCA_IRQ));

    /* IDE buses */
    if (!macio_realize_ide(s, &ns->ide[0],
                           qdev_get_gpio_in(pic_dev, NEWWORLD_IDE0_IRQ),
                           qdev_get_gpio_in(pic_dev, NEWWORLD_IDE0_DMA_IRQ),
                           0x0b, errp)) {
        return;
    }

    if (!macio_realize_ide(s, &ns->ide[1],
                           qdev_get_gpio_in(pic_dev, NEWWORLD_IDE1_IRQ),
                           qdev_get_gpio_in(pic_dev, NEWWORLD_IDE1_DMA_IRQ),
                           0x0d, errp)) {
        return;
    }

    /* Timer */
    timer_memory = g_new(MemoryRegion, 1);
    memory_region_init_io(timer_memory, OBJECT(s), &timer_ops, NULL, "timer",
                          0x1000);
    memory_region_add_subregion(&s->bar, 0x15000, timer_memory);

    if (ns->has_pmu) {
        /* GPIOs */
        if (!qdev_realize(DEVICE(&ns->gpio), BUS(&s->macio_bus), errp)) {
            return;
        }
        sbd = SYS_BUS_DEVICE(&ns->gpio);
        sysbus_connect_irq(sbd, 1, qdev_get_gpio_in(pic_dev,
                           NEWWORLD_EXTING_GPIO1));
        sysbus_connect_irq(sbd, 9, qdev_get_gpio_in(pic_dev,
                           NEWWORLD_EXTING_GPIO9));
        memory_region_add_subregion(&s->bar, 0x50,
                                    sysbus_mmio_get_region(sbd, 0));

        /* PMU */
        object_initialize_child(OBJECT(s), "pmu", &s->pmu, TYPE_VIA_PMU);
        object_property_set_link(OBJECT(&s->pmu), "gpio", OBJECT(sbd),
                                 &error_abort);
        qdev_prop_set_bit(DEVICE(&s->pmu), "has-adb", ns->has_adb);
        if (!qdev_realize(DEVICE(&s->pmu), BUS(&s->macio_bus), errp)) {
            return;
        }
        sbd = SYS_BUS_DEVICE(&s->pmu);
        sysbus_connect_irq(sbd, 0, qdev_get_gpio_in(pic_dev, NEWWORLD_PMU_IRQ));
        memory_region_add_subregion(&s->bar, 0x16000,
                                    sysbus_mmio_get_region(sbd, 0));
    } else {
        object_unparent(OBJECT(&ns->gpio));

        /* CUDA */
        object_initialize_child(OBJECT(s), "cuda", &s->cuda, TYPE_CUDA);
        qdev_prop_set_uint64(DEVICE(&s->cuda), "timebase-frequency",
                             s->frequency);

        if (!qdev_realize(DEVICE(&s->cuda), BUS(&s->macio_bus), errp)) {
            return;
        }
        sbd = SYS_BUS_DEVICE(&s->cuda);
        sysbus_connect_irq(sbd, 0, qdev_get_gpio_in(pic_dev, NEWWORLD_CUDA_IRQ));
        memory_region_add_subregion(&s->bar, 0x16000,
                                    sysbus_mmio_get_region(sbd, 0));
    }
}

static void macio_newworld_init(Object *obj)
{
    MacIOState *s = MACIO(obj);
    NewWorldMacIOState *ns = NEWWORLD_MACIO(obj);
    int i;

    object_initialize_child(obj, "pic", &ns->pic, TYPE_OPENPIC);

    object_initialize_child(obj, "gpio", &ns->gpio, TYPE_MACIO_GPIO);

    for (i = 0; i < 2; i++) {
        macio_init_ide(s, &ns->ide[i], i);
    }
}

static void macio_instance_init(Object *obj)
{
    MacIOState *s = MACIO(obj);

    memory_region_init(&s->bar, obj, "macio", 0x80000);

    qbus_init(&s->macio_bus, sizeof(s->macio_bus), TYPE_MACIO_BUS,
              DEVICE(obj), "macio.0");

    object_initialize_child(obj, "dbdma", &s->dbdma, TYPE_MAC_DBDMA);

    object_initialize_child(obj, "escc", &s->escc, TYPE_ESCC);
}

static const VMStateDescription vmstate_macio_oldworld = {
    .name = "macio-oldworld",
    .version_id = 0,
    .minimum_version_id = 0,
    .fields = (const VMStateField[]) {
        VMSTATE_PCI_DEVICE(parent_obj.parent, OldWorldMacIOState),
        VMSTATE_END_OF_LIST()
    }
};

static void macio_oldworld_class_init(ObjectClass *oc, const void *data)
{
    PCIDeviceClass *pdc = PCI_DEVICE_CLASS(oc);
    DeviceClass *dc = DEVICE_CLASS(oc);

    pdc->realize = macio_oldworld_realize;
    pdc->device_id = PCI_DEVICE_ID_APPLE_343S1201;
    dc->vmsd = &vmstate_macio_oldworld;
}

/*
 * Grand Central (343S1125), the TNT and Network Server I/O controller.
 * It has no BAR: the board decodes its 128 KB island at a fixed
 * address through the bridge's pass-through window, so the machine maps
 * the "bar" region itself.
 */
/* 53C94 on DBDMA channel 0: the chip's DMA port is paced by the descriptor */
static DBDMA_io *gc_esp_io(GrandCentralMacIOState *gs)
{
    DBDMA_io *io = gs->esp_io;

    if (!io || !io->processing || gs->esp_io_done) {
        return NULL;
    }
    return io;
}

static uint32_t gc_esp_dma_avail(void *opaque)
{
    DBDMA_io *io = gc_esp_io(opaque);

    return io ? io->len : 0;
}

static void gc_esp_dma_advance(GrandCentralMacIOState *gs, DBDMA_io *io,
                               int len)
{
    io->addr += len;
    io->len -= len;
    if (io->len == 0) {
        gs->esp_io_done = true;
        qemu_bh_schedule(gs->esp_dma_bh);
    }
}

static void gc_esp_dma_read(void *opaque, uint8_t *buf, int len)
{
    GrandCentralMacIOState *gs = opaque;
    DBDMA_io *io = gc_esp_io(gs);

    if (!io || !io->is_dma_out || len > io->len) {
        qemu_log_mask(LOG_GUEST_ERROR, "53c94: DMA read without an output "
                      "descriptor\n");
        memset(buf, 0, len);
        return;
    }
    dma_memory_read(io->as, io->addr, buf, len,
                    MEMTXATTRS_UNSPECIFIED);
    gc_esp_dma_advance(gs, io, len);
}

static void gc_esp_dma_write(void *opaque, uint8_t *buf, int len)
{
    GrandCentralMacIOState *gs = opaque;
    DBDMA_io *io = gc_esp_io(gs);

    if (!io || io->is_dma_out || len > io->len) {
        qemu_log_mask(LOG_GUEST_ERROR, "53c94: DMA write without an input "
                      "descriptor\n");
        return;
    }
    dma_memory_write(io->as, io->addr, buf, len,
                     MEMTXATTRS_UNSPECIFIED);
    gc_esp_dma_advance(gs, io, len);
}

/* Descriptor completion runs outside the chip's transfer loop */
static void gc_esp_dma_end(GrandCentralMacIOState *gs)
{
    DBDMA_io *io = gs->esp_io;

    if (!gs->esp_io_done) {
        return;
    }
    gs->esp_io_done = false;
    gs->esp_io = NULL;
    if (io->processing) {
        io->dma_end(io);
    }
}

static void gc_esp_dma_bh(void *opaque)
{
    gc_esp_dma_end(opaque);
}

static void gc_esp_dma_rw(DBDMA_io *io)
{
    GrandCentralMacIOState *gs = io->opaque;

    gs->esp_io = io;
    gs->esp_io_done = false;
    if (io->len == 0) {
        gs->esp_io_done = true;
        qemu_bh_schedule(gs->esp_dma_bh);
        return;
    }
    esp_dma_ready(&gs->esp.esp);
}

static void gc_esp_dma_flush(DBDMA_io *io)
{
    GrandCentralMacIOState *gs = io->opaque;

    gc_esp_dma_end(gs);
}

static void macio_grandcentral_realize(PCIDevice *d, Error **errp)
{
    MacIOState *s = MACIO(d);
    GrandCentralMacIOState *gs = GRAND_CENTRAL_MACIO(d);
    DeviceState *pic_dev = DEVICE(&gs->pic);
    SysBusDevice *sbd;

    if (!macio_common_children(d, errp)) {
        return;
    }
    pci_set_byte(&d->config[PCI_REVISION_ID], 2);
    pci_set_long(&d->config[PCI_SUBSYSTEM_VENDOR_ID], 0);

    if (!qdev_realize(DEVICE(&gs->pic), BUS(&s->macio_bus), errp)) {
        return;
    }
    sbd = SYS_BUS_DEVICE(&gs->pic);
    memory_region_add_subregion(&s->bar, 0x20,
                                sysbus_mmio_get_region(sbd, 0));

    qdev_prop_set_uint64(DEVICE(&s->cuda), "timebase-frequency",
                         s->frequency);
    if (!qdev_realize(DEVICE(&s->cuda), BUS(&s->macio_bus), errp)) {
        return;
    }
    sbd = SYS_BUS_DEVICE(&s->cuda);
    memory_region_add_subregion(&s->bar, 0x16000,
                                sysbus_mmio_get_region(sbd, 0));
    sysbus_connect_irq(sbd, 0, qdev_get_gpio_in(pic_dev, GC_CUDA_IRQ));

    sbd = SYS_BUS_DEVICE(&s->escc);
    sysbus_connect_irq(sbd, 0, qdev_get_gpio_in(pic_dev, GC_ESCCB_IRQ));
    sysbus_connect_irq(sbd, 1, qdev_get_gpio_in(pic_dev, GC_ESCCA_IRQ));
    sysbus_connect_irq(sbd, 2,
                       qdev_get_gpio_in(pic_dev, OLDWORLD_ESCCB_TX_DMA_IRQ));
    sysbus_connect_irq(sbd, 3,
                       qdev_get_gpio_in(pic_dev, OLDWORLD_ESCCB_RX_DMA_IRQ));
    sysbus_connect_irq(sbd, 4,
                       qdev_get_gpio_in(pic_dev, OLDWORLD_ESCCA_TX_DMA_IRQ));
    sysbus_connect_irq(sbd, 5,
                       qdev_get_gpio_in(pic_dev, OLDWORLD_ESCCA_RX_DMA_IRQ));
    escc_register_dma(&s->escc, &s->dbdma);

    /* 53C94, external SCSI bus */
    gs->esp.it_shift = 4;
    gs->esp.esp.dma_memory_read = gc_esp_dma_read;
    gs->esp.esp.dma_memory_write = gc_esp_dma_write;
    gs->esp.esp.dma_memory_avail = gc_esp_dma_avail;
    gs->esp.esp.dma_opaque = gs;
    gs->esp.esp.dma_enabled = 1;
    if (!qdev_realize(DEVICE(&gs->esp), BUS(&s->macio_bus), errp)) {
        return;
    }
    sbd = SYS_BUS_DEVICE(&gs->esp);
    memory_region_add_subregion(&s->bar, 0x10000,
                                sysbus_mmio_get_region(sbd, 0));
    sysbus_connect_irq(sbd, 0, qdev_get_gpio_in(pic_dev, GC_SCSI_IRQ));
    gs->esp_dma_bh = qemu_bh_new_guarded(gc_esp_dma_bh, gs,
                                         &DEVICE(gs)->mem_reentrancy_guard);
    DBDMA_register_channel(&s->dbdma, 0,
                           qdev_get_gpio_in(pic_dev, GC_SCSI_DMA_IRQ),
                           gc_esp_dma_rw, gc_esp_dma_flush, gs);

    if (!qdev_realize(DEVICE(&gs->mace), BUS(&s->macio_bus), errp)) {
        return;
    }
    sbd = SYS_BUS_DEVICE(&gs->mace);
    memory_region_add_subregion(&s->bar, 0x11000,
                                sysbus_mmio_get_region(sbd, 0));
    sysbus_connect_irq(sbd, 0, qdev_get_gpio_in(pic_dev, GC_MACE_IRQ));
    sysbus_connect_irq(sbd, 1, qdev_get_gpio_in(pic_dev, GC_ENET_TX_DMA_IRQ));
    sysbus_connect_irq(sbd, 2, qdev_get_gpio_in(pic_dev, GC_ENET_RX_DMA_IRQ));
    mace_register_dma(&gs->mace, &s->dbdma, 2, 3);

    if (!qdev_realize(DEVICE(&gs->awacs), BUS(&s->macio_bus), errp)) {
        return;
    }
    sbd = SYS_BUS_DEVICE(&gs->awacs);
    memory_region_add_subregion(&s->bar, 0x14000,
                                sysbus_mmio_get_region(sbd, 0));
    sysbus_connect_irq(sbd, 0, qdev_get_gpio_in(pic_dev, GC_AWACS_IRQ));
    sysbus_connect_irq(sbd, 1,
                       qdev_get_gpio_in(pic_dev, OLDWORLD_AWACS_DMA_IRQ));
    awacs_register_dma(&gs->awacs, &s->dbdma);

    if (!qdev_realize(DEVICE(&gs->swim), BUS(&s->macio_bus), errp)) {
        return;
    }
    sbd = SYS_BUS_DEVICE(&gs->swim);
    memory_region_add_subregion(&s->bar, 0x15000,
                                sysbus_mmio_get_region(sbd, 0));
    sysbus_connect_irq(sbd, 0, qdev_get_gpio_in(pic_dev, GC_SWIM_IRQ));
    sysbus_connect_irq(sbd, 1, qdev_get_gpio_in(pic_dev, GC_SWIM_DMA_IRQ));
    swim3_register_dma(&gs->swim, &s->dbdma);

    if (!qdev_realize(DEVICE(&gs->gbus), BUS(&s->macio_bus), errp)) {
        return;
    }
    sbd = SYS_BUS_DEVICE(&gs->gbus);
    memory_region_add_subregion(&s->bar, ANS_GBUS_OFFSET,
                                sysbus_mmio_get_region(sbd, 0));
}

static void macio_grandcentral_init(Object *obj)
{
    MacIOState *s = MACIO(obj);
    GrandCentralMacIOState *gs = GRAND_CENTRAL_MACIO(obj);

    /* the island decodes 128 KB */
    memory_region_set_size(&s->bar, 0x20000);
    object_initialize_child(obj, "pic", &gs->pic, TYPE_GRAND_CENTRAL_PIC);
    object_initialize_child(obj, "cuda", &s->cuda, TYPE_CUDA);
    object_initialize_child(obj, "esp", &gs->esp, TYPE_SYSBUS_ESP);
    object_initialize_child(obj, "mace", &gs->mace, TYPE_MACE);
    object_initialize_child(obj, "awacs", &gs->awacs, TYPE_AWACS);
    object_initialize_child(obj, "swim", &gs->swim, TYPE_SWIM3);
    object_initialize_child(obj, "gbus", &gs->gbus, TYPE_ANS_GBUS);
}

static const VMStateDescription vmstate_macio_grandcentral = {
    .name = "macio-grandcentral",
    .version_id = 0,
    .minimum_version_id = 0,
    .fields = (const VMStateField[]) {
        VMSTATE_PCI_DEVICE(parent_obj.parent, GrandCentralMacIOState),
        VMSTATE_END_OF_LIST()
    }
};

static void macio_grandcentral_class_init(ObjectClass *oc, const void *data)
{
    PCIDeviceClass *pdc = PCI_DEVICE_CLASS(oc);
    DeviceClass *dc = DEVICE_CLASS(oc);

    pdc->realize = macio_grandcentral_realize;
    pdc->device_id = PCI_DEVICE_ID_APPLE_GRAND_CENTRAL;
    dc->vmsd = &vmstate_macio_grandcentral;
}

static const VMStateDescription vmstate_macio_newworld = {
    .name = "macio-newworld",
    .version_id = 0,
    .minimum_version_id = 0,
    .fields = (const VMStateField[]) {
        VMSTATE_PCI_DEVICE(parent_obj.parent, NewWorldMacIOState),
        VMSTATE_END_OF_LIST()
    }
};

static const Property macio_newworld_properties[] = {
    DEFINE_PROP_BOOL("has-pmu", NewWorldMacIOState, has_pmu, false),
    DEFINE_PROP_BOOL("has-adb", NewWorldMacIOState, has_adb, false),
};

static void macio_newworld_class_init(ObjectClass *oc, const void *data)
{
    PCIDeviceClass *pdc = PCI_DEVICE_CLASS(oc);
    DeviceClass *dc = DEVICE_CLASS(oc);

    pdc->realize = macio_newworld_realize;
    pdc->device_id = PCI_DEVICE_ID_APPLE_UNI_N_KEYL;
    dc->vmsd = &vmstate_macio_newworld;
    device_class_set_props(dc, macio_newworld_properties);
}

static const Property macio_properties[] = {
    DEFINE_PROP_UINT64("frequency", MacIOState, frequency, 0),
};

static void macio_class_init(ObjectClass *klass, const void *data)
{
    PCIDeviceClass *k = PCI_DEVICE_CLASS(klass);
    DeviceClass *dc = DEVICE_CLASS(klass);

    k->vendor_id = PCI_VENDOR_ID_APPLE;
    k->class_id = PCI_CLASS_OTHERS << 8;
    device_class_set_props(dc, macio_properties);
    set_bit(DEVICE_CATEGORY_BRIDGE, dc->categories);
}

static const TypeInfo macio_bus_info = {
    .name = TYPE_MACIO_BUS,
    .parent = TYPE_SYSTEM_BUS,
    .instance_size = sizeof(MacIOBusState),
};

static const TypeInfo macio_oldworld_type_info = {
    .name          = TYPE_OLDWORLD_MACIO,
    .parent        = TYPE_MACIO,
    .instance_size = sizeof(OldWorldMacIOState),
    .instance_init = macio_oldworld_init,
    .class_init    = macio_oldworld_class_init,
};

static const TypeInfo macio_newworld_type_info = {
    .name          = TYPE_NEWWORLD_MACIO,
    .parent        = TYPE_MACIO,
    .instance_size = sizeof(NewWorldMacIOState),
    .instance_init = macio_newworld_init,
    .class_init    = macio_newworld_class_init,
};

static const TypeInfo macio_grandcentral_type_info = {
    .name          = TYPE_GRAND_CENTRAL_MACIO,
    .parent        = TYPE_MACIO,
    .instance_size = sizeof(GrandCentralMacIOState),
    .instance_init = macio_grandcentral_init,
    .class_init    = macio_grandcentral_class_init,
};

static const TypeInfo macio_type_info = {
    .name          = TYPE_MACIO,
    .parent        = TYPE_PCI_DEVICE,
    .instance_size = sizeof(MacIOState),
    .instance_init = macio_instance_init,
    .abstract      = true,
    .class_init    = macio_class_init,
    .interfaces = (const InterfaceInfo[]) {
        { INTERFACE_CONVENTIONAL_PCI_DEVICE },
        { },
    },
};

static void macio_register_types(void)
{
    type_register_static(&macio_bus_info);
    type_register_static(&macio_type_info);
    type_register_static(&macio_oldworld_type_info);
    type_register_static(&macio_newworld_type_info);
    type_register_static(&macio_grandcentral_type_info);
}

type_init(macio_register_types)
