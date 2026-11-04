/*
 * AMD Am79C940 MACE Ethernet controller
 *
 * SPDX-License-Identifier: GPL-2.0-or-later
 */

#ifndef HW_NET_MACE_H
#define HW_NET_MACE_H

#include "hw/core/sysbus.h"
#include "hw/ppc/mac_dbdma.h"
#include "net/net.h"
#include "qom/object.h"

#define TYPE_MACE "mace"
OBJECT_DECLARE_SIMPLE_TYPE(MACEState, MACE)

#define MACE_FRAME_MAX  1536

struct MACEState {
    SysBusDevice parent_obj;

    MemoryRegion mem;
    NICState *nic;
    NICConf conf;
    qemu_irq irq;
    qemu_irq irq_tx_dma;
    qemu_irq irq_rx_dma;
    DBDMAState *dbdma;
    int tx_chan;
    int rx_chan;

    uint8_t regs[32];
    uint8_t addr_ptr;
    uint8_t padr[6];
    uint8_t ladrf[8];

    /* transmit frame gathered from the DMA channel */
    uint8_t tx_frame[MACE_FRAME_MAX];
    int tx_len;

    /* received frame, FCS and status in the FIFO, not yet moved by DMA */
    uint8_t rx_frame[MACE_FRAME_MAX + 8];
    int rx_len;
    int rx_pos;
    DBDMA_io *rx_io;
};

/* the controller's transmit and receive DBDMA channels */
void mace_register_dma(MACEState *s, DBDMAState *dbdma, int tx_chan,
                       int rx_chan);

#endif
