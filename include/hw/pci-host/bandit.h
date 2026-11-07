/*
 * Apple Bandit PCI host bridge
 *
 * SPDX-License-Identifier: GPL-2.0-or-later
 */

#ifndef HW_PCI_HOST_BANDIT_H
#define HW_PCI_HOST_BANDIT_H

#include "hw/pci/pci_host.h"
#include "qom/object.h"

#define TYPE_BANDIT_PCI_HOST_BRIDGE "bandit-pcihost"
OBJECT_DECLARE_SIMPLE_TYPE(BanditHostState, BANDIT_PCI_HOST_BRIDGE)

#define TYPE_BANDIT_PCI_DEVICE "bandit"

#define BANDIT_NUM_IRQS      32
#define BANDIT_IO_SIZE       0x00800000
#define BANDIT_CFG_ADDR      0x00800000
#define BANDIT_CFG_DATA      0x00C00000

/*
 * sysbus mmio regions:
 *   0  config address port   (map at base + 0x800000)
 *   1  config data port      (map at base + 0xC00000)
 *   2  PCI I/O window, 8 MB  (map at base)
 *   3  PCI memory pass-through, 16 MB (map at base + 16 MB)
 *   4  PCI memory, 256 MB    (map at the "mem-base" property)
 */
/*
 * A CPU-side window with the bridge's little-endian byte-lane reversal:
 * an N-byte access at offset o reaches PCI at o ^ (8 - N) with its
 * value taken as little-endian.
 */
typedef struct BanditLane {
    MemoryRegion outer;
    MemoryRegion straight;
    MemoryRegion reversed;
    MemoryRegion root;
    MemoryRegion target;
    AddressSpace as;
} BanditLane;

#define BANDIT_NUM_LANES 5

struct BanditHostState {
    PCIHostState parent_obj;

    uint32_t base;
    uint32_t mem_base;
    uint32_t addr_select;
    uint32_t mode_select;

    MemoryRegion pci_mmio;
    MemoryRegion pci_io;
    MemoryRegion io_window;
    MemoryRegion io_alias[BANDIT_IO_SIZE / 0x10000];
    MemoryRegion pass_through;
    MemoryRegion mem_window;
    qemu_irq irqs[BANDIT_NUM_IRQS];

    bool lanes_reversed;
    BanditLane lanes[BANDIT_NUM_LANES];
    /* bus-master view of host memory */
    MemoryRegion dma_root;
    MemoryRegion dma_straight;
    MemoryRegion dma_reversed;
    AddressSpace dma_as;
};

void bandit_set_lanes_reversed(BanditHostState *s, bool on);

#endif
