/*
 * Apple Network Server 700/200 ("Shiner HE")
 *
 * SPDX-License-Identifier: GPL-2.0-or-later
 *
 * A Power Macintosh 9500 board with the Macintosh removed: a 200 MHz
 * PowerPC 604e on a 50 MHz bus, Hammerhead memory controller with a
 * 1 MB L2 cache DIMM, two Bandit PCI bridges, Grand Central I/O with
 * the Network Server's GBus devices (front-panel LCD, keyswitch, board
 * registers), two 53C825A fast/wide SCSI controllers and a Cirrus
 * 54M30 display controller on the first bridge.  The production ROM is
 * Open Firmware 1.1.22.
 *
 * Physical map:
 *   0x00000000  DRAM, decoded by the Hammerhead bank registers
 *   0x80000000  Bandit 1 PCI memory (256 MB)
 *   0x90000000  Bandit 2 PCI memory (256 MB)
 *   0xF2000000  Bandit 1: PCI I/O (8 MB), config ports +0x800000/+0xC00000
 *   0xF3000000  Bandit 1 pass-through memory; Grand Central at its base
 *   0xF4000000  Bandit 2: PCI I/O, config ports
 *   0xF5000000  Bandit 2 pass-through memory
 *   0xF8000000  Hammerhead registers
 *   0xFFC00000  ROM, 4 MB (reset vector 0xFFF00100)
 */

#include "qemu/osdep.h"
#include "qemu/datadir.h"
#include "qemu/units.h"
#include "qemu/error-report.h"
#include "qapi/error.h"
#include "qapi/visitor.h"
#include "hw/ppc/ppc.h"
#include "hw/core/boards.h"
#include "hw/core/loader.h"
#include "hw/core/qdev-properties.h"
#include "hw/core/qdev-properties-system.h"
#include "hw/input/adb.h"
#include "hw/pci/pci.h"
#include "hw/pci-host/bandit.h"
#include "hw/misc/hammerhead.h"
#include "hw/misc/macio/macio.h"
#include "hw/misc/macio/cuda.h"
#include "hw/char/escc.h"
#include "chardev/char.h"
#include "system/system.h"
#include "system/reset.h"
#include "target/ppc/cpu.h"

#define ANS_CLOCKFREQ   200000000UL
#define ANS_BUSFREQ     50000000UL
#define ANS_TBFREQ      (ANS_BUSFREQ / 4)

#define ANS_ROM_BASE    0xffc00000
#define ANS_ROM_SIZE    (4 * MiB)
#define ANS_HH_BASE     0xf8000000
#define ANS_BANDIT1     0xf2000000
#define ANS_BANDIT2     0xf4000000
#define ANS_GC_BASE     0xf3000000

#define TYPE_ANS700_MACHINE MACHINE_TYPE_NAME("ans700")
OBJECT_DECLARE_SIMPLE_TYPE(ANSMachineState, ANS700_MACHINE)

struct ANSMachineState {
    MachineState parent_obj;

    PowerPCCPU *cpu;
    char *keyswitch;
    char *nvram;
    char *lcd;
    uint32_t power_supplies;
    uint32_t pll_cfg;
};

/*
 * Grand Central external interrupt lines per PCI device: the Network
 * Server moves the fast/wide SCSI controllers onto the Bandit positions
 * and leaves slot 3 on EXT5 although it sits on the second bridge.
 */
static const struct {
    int bandit;
    int slot;
    int ext;
} ans_pci_irq_map[] = {
    { 1, 13, 3 },   /* slot 1 */
    { 1, 14, 4 },   /* slot 2 */
    { 1, 17, 2 },   /* 53C825A #0 */
    { 1, 18, 6 },   /* 53C825A #1 */
    { 2, 13, 5 },   /* slot 3 */
    { 2, 14, 7 },   /* slot 4 */
    { 2, 15, 8 },   /* slot 5 */
    { 2, 16, 9 },   /* slot 6 */
};

static void ans_cpu_reset(void *opaque)
{
    ANSMachineState *ams = opaque;
    PowerPCCPU *cpu = ams->cpu;

    cpu_ppc_tb_reset(&cpu->env);
    cpu_reset(CPU(cpu));
    /* PLL_CFG straps: the bus-to-core ratio of the CPU card */
    cpu->env.spr[SPR_HID1] = (target_ulong)ams->pll_cfg << 28;
}

static DeviceState *ans_bandit(uint32_t base, uint32_t mem_base,
                               uint32_t addr_select)
{
    DeviceState *dev = qdev_new(TYPE_BANDIT_PCI_HOST_BRIDGE);
    SysBusDevice *sbd = SYS_BUS_DEVICE(dev);

    qdev_prop_set_uint32(dev, "base", base);
    qdev_prop_set_uint32(dev, "mem-base", mem_base);
    qdev_prop_set_uint32(dev, "addr-select", addr_select);
    sysbus_realize_and_unref(sbd, &error_fatal);

    sysbus_mmio_map(sbd, 0, base + BANDIT_CFG_ADDR);
    sysbus_mmio_map(sbd, 1, base + BANDIT_CFG_DATA);
    sysbus_mmio_map(sbd, 2, base);
    sysbus_mmio_map(sbd, 3, base + 16 * MiB);
    sysbus_mmio_map(sbd, 4, mem_base);
    return dev;
}

static void ans_init(MachineState *machine)
{
    ANSMachineState *ams = ANS700_MACHINE(machine);
    MemoryRegion *sysmem = get_system_memory();
    MemoryRegion *rom = g_new(MemoryRegion, 1);
    DeviceState *hh, *b1, *b2, *dev, *pic_dev;
    BanditHostState *bandit1;
    PCIBus *bus1;
    MacIOState *macio;
    Object *gc;
    BusState *adb_bus;
    char *filename;
    int size, i;

    if (!machine->firmware) {
        error_report("ans700 needs the Network Server ROM: -bios <file>");
        exit(1);
    }
    if (machine->ram_size > 512 * MiB) {
        error_report("the Network Server ROM decodes at most 512 MB of RAM");
        exit(1);
    }

    ams->cpu = POWERPC_CPU(cpu_create(machine->cpu_type));
    cpu_ppc_tb_init(&ams->cpu->env, ANS_TBFREQ);
    qemu_register_reset(ans_cpu_reset, ams);

    /* Hammerhead: registers and the DRAM bank decode */
    hh = qdev_new(TYPE_HAMMERHEAD);
    object_property_set_link(OBJECT(hh), "ram", OBJECT(machine->ram),
                             &error_abort);
    qdev_prop_set_uint32(hh, "l2-kb", 1024);
    sysbus_realize_and_unref(SYS_BUS_DEVICE(hh), &error_fatal);
    sysbus_mmio_map(SYS_BUS_DEVICE(hh), 0, ANS_HH_BASE);
    sysbus_mmio_map(SYS_BUS_DEVICE(hh), 1, 0);

    /* ROM */
    memory_region_init_rom(rom, NULL, "ans.rom", ANS_ROM_SIZE, &error_fatal);
    memory_region_add_subregion(sysmem, ANS_ROM_BASE, rom);
    filename = qemu_find_file(QEMU_FILE_TYPE_BIOS, machine->firmware);
    if (!filename) {
        error_report("cannot find ROM '%s'", machine->firmware);
        exit(1);
    }
    size = load_image_targphys(filename, ANS_ROM_BASE, ANS_ROM_SIZE,
                               &error_fatal);
    if (size != ANS_ROM_SIZE) {
        error_report("ROM '%s' is not a 4 MB image", filename);
        exit(1);
    }
    g_free(filename);

    /* the two Bandits */
    b1 = ans_bandit(ANS_BANDIT1, 0x80000000, 0x0100000c);
    b2 = ans_bandit(ANS_BANDIT2, 0x90000000, 0x02000030);
    bandit1 = BANDIT_PCI_HOST_BRIDGE(b1);
    bus1 = PCI_HOST_BRIDGE(b1)->bus;

    /* Grand Central at IDSEL 16, its island at the pass-through base */
    gc = OBJECT(pci_new(PCI_DEVFN(16, 0), TYPE_GRAND_CENTRAL_MACIO));
    macio = MACIO(gc);
    qdev_prop_set_uint64(DEVICE(gc), "frequency", ANS_TBFREQ);
    dev = DEVICE(object_resolve_path_component(gc, "escc"));
    qdev_prop_set_chr(dev, "chrA", serial_hd(0));
    qdev_prop_set_chr(dev, "chrB", serial_hd(1));
    dev = DEVICE(object_resolve_path_component(gc, "cuda"));
    qdev_prop_set_bit(dev, "default-pram", false);
    dev = DEVICE(object_resolve_path_component(gc, "gbus"));
    qdev_prop_set_string(dev, "keyswitch", ams->keyswitch);
    qdev_prop_set_bit(dev, "two-supplies", ams->power_supplies == 2);
    if (ams->nvram) {
        qdev_prop_set_string(dev, "nvram-file", ams->nvram);
    }
    if (ams->lcd) {
        Chardev *chr = qemu_chr_find(ams->lcd);

        if (!chr) {
            error_report("lcd: no chardev '%s'", ams->lcd);
            exit(1);
        }
        qdev_prop_set_chr(dev, "lcd", chr);
    }
    pci_realize_and_unref(PCI_DEVICE(gc), bus1, &error_fatal);
    memory_region_add_subregion_overlap(&bandit1->pci_mmio, ANS_GC_BASE,
                                        &macio->bar, 1);

    pic_dev = DEVICE(object_resolve_path_component(gc, "pic"));
    sysbus_connect_irq(SYS_BUS_DEVICE(pic_dev), 0,
                       qdev_get_gpio_in(DEVICE(ams->cpu), PPC6xx_INPUT_INT));
    for (i = 0; i < ARRAY_SIZE(ans_pci_irq_map); i++) {
        qdev_connect_gpio_out(ans_pci_irq_map[i].bandit == 1 ? b1 : b2,
                              ans_pci_irq_map[i].slot,
                              qdev_get_gpio_in(pic_dev,
                                    GC_EXT_IRQ(ans_pci_irq_map[i].ext)));
    }

    /* built-in PCI devices on Bandit 1 */
    pci_create_simple(bus1, PCI_DEVFN(15, 0), "cirrus-54m30");
    pci_create_simple(bus1, PCI_DEVFN(17, 0), "lsi53c825a");
    pci_create_simple(bus1, PCI_DEVFN(18, 0), "lsi53c825a");

    /* ADB keyboard and mouse on Cuda */
    dev = DEVICE(object_resolve_path_component(gc, "cuda"));
    adb_bus = qdev_get_child_bus(dev, "adb.0");
    qdev_realize_and_unref(qdev_new(TYPE_ADB_KEYBOARD), adb_bus,
                           &error_fatal);
    qdev_realize_and_unref(qdev_new(TYPE_ADB_MOUSE), adb_bus, &error_fatal);
}

static char *ans_get_keyswitch(Object *obj, Error **errp)
{
    return g_strdup(ANS700_MACHINE(obj)->keyswitch);
}

static void ans_set_keyswitch(Object *obj, const char *value, Error **errp)
{
    ANSMachineState *ams = ANS700_MACHINE(obj);

    if (strcmp(value, "normal") && strcmp(value, "locked") &&
        strcmp(value, "service")) {
        error_setg(errp, "keyswitch must be normal, locked or service");
        return;
    }
    g_free(ams->keyswitch);
    ams->keyswitch = g_strdup(value);
}

static char *ans_get_nvram(Object *obj, Error **errp)
{
    return g_strdup(ANS700_MACHINE(obj)->nvram);
}

static void ans_set_nvram(Object *obj, const char *value, Error **errp)
{
    ANSMachineState *ams = ANS700_MACHINE(obj);

    g_free(ams->nvram);
    ams->nvram = g_strdup(value);
}

static char *ans_get_lcd(Object *obj, Error **errp)
{
    return g_strdup(ANS700_MACHINE(obj)->lcd);
}

static void ans_set_lcd(Object *obj, const char *value, Error **errp)
{
    ANSMachineState *ams = ANS700_MACHINE(obj);

    g_free(ams->lcd);
    ams->lcd = g_strdup(value);
}

static void ans_get_uint32(Object *obj, Visitor *v, const char *name,
                           void *opaque, Error **errp)
{
    visit_type_uint32(v, name, opaque, errp);
}

static void ans_set_power_supplies(Object *obj, Visitor *v, const char *name,
                                   void *opaque, Error **errp)
{
    uint32_t value;

    if (!visit_type_uint32(v, name, &value, errp)) {
        return;
    }
    if (value != 1 && value != 2) {
        error_setg(errp, "power-supplies must be 1 or 2");
        return;
    }
    *(uint32_t *)opaque = value;
}

static void ans_set_pll_cfg(Object *obj, Visitor *v, const char *name,
                            void *opaque, Error **errp)
{
    uint32_t value;

    if (!visit_type_uint32(v, name, &value, errp)) {
        return;
    }
    if (value > 15) {
        error_setg(errp, "pll-cfg is a 4-bit strap");
        return;
    }
    *(uint32_t *)opaque = value;
}

static void ans_instance_init(Object *obj)
{
    ANSMachineState *ams = ANS700_MACHINE(obj);

    ams->keyswitch = g_strdup("normal");
    ams->power_supplies = 1;
    ams->pll_cfg = 0xa;

    object_property_add_str(obj, "keyswitch", ans_get_keyswitch,
                            ans_set_keyswitch);
    object_property_set_description(obj, "keyswitch",
                                    "Front keyswitch: normal, locked or "
                                    "service");
    object_property_add_str(obj, "nvram", ans_get_nvram, ans_set_nvram);
    object_property_set_description(obj, "nvram",
                                    "File holding the 8 KB NVRAM "
                                    "(default: blank, not saved)");
    object_property_add_str(obj, "lcd", ans_get_lcd, ans_set_lcd);
    object_property_set_description(obj, "lcd",
                                    "Chardev id receiving the front-panel "
                                    "LCD lines");
    object_property_add(obj, "power-supplies", "uint32", ans_get_uint32,
                        ans_set_power_supplies, NULL, &ams->power_supplies);
    object_property_set_description(obj, "power-supplies",
                                    "Number of power supplies (1 or 2)");
    object_property_add(obj, "pll-cfg", "uint32", ans_get_uint32,
                        ans_set_pll_cfg, NULL, &ams->pll_cfg);
    object_property_set_description(obj, "pll-cfg",
                                    "604e PLL_CFG strap (HID1[0:3])");
}

static void ans_machine_class_init(ObjectClass *oc, const void *data)
{
    MachineClass *mc = MACHINE_CLASS(oc);

    mc->desc = "Apple Network Server 700/200";
    mc->init = ans_init;
    mc->max_cpus = 1;
    mc->default_cpu_type = POWERPC_CPU_TYPE_NAME("604e_v2.4");
    mc->default_ram_size = 64 * MiB;
    mc->default_ram_id = "ans.ram";
    mc->block_default_type = IF_SCSI;
    mc->no_parallel = 1;
    mc->default_display = "none";
}

static const TypeInfo ans_machine_info = {
    .name          = TYPE_ANS700_MACHINE,
    .parent        = TYPE_MACHINE,
    .instance_size = sizeof(ANSMachineState),
    .instance_init = ans_instance_init,
    .class_init    = ans_machine_class_init,
};

static void ans_machine_register_types(void)
{
    type_register_static(&ans_machine_info);
}

type_init(ans_machine_register_types)
