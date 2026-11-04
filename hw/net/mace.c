/*
 * AMD Am79C940 MACE Ethernet controller
 *
 * SPDX-License-Identifier: GPL-2.0-or-later
 *
 * Thirty-two byte registers on 0x10 centres.  The FIFOs are emptied and
 * filled by two DBDMA channels: a transmit frame is the bytes of one
 * OUTPUT_MORE.. OUTPUT_LAST group (the last byte carries EOF), and a
 * received frame goes to memory followed by its FCS (unless stripped)
 * and the four receive status bytes, ending the input command it
 * finishes in.
 */

#include "qemu/osdep.h"
#include "qemu/log.h"
#include "qemu/module.h"
#include "qapi/error.h"
#include "hw/core/irq.h"
#include "hw/core/qdev-properties.h"
#include "hw/net/mace.h"
#include "migration/vmstate.h"
#include "net/eth.h"
#include "system/dma.h"
#include "trace.h"

enum {
    R_RCVFIFO = 0, R_XMTFIFO, R_XMTFC, R_XMTFS, R_XMTRC, R_RCVFC, R_RCVFS,
    R_FIFOFC, R_IR, R_IMR, R_PR, R_BIUCC, R_FIFOCC, R_MACCC, R_PLSCC,
    R_PHYCC, R_CHIPID0, R_CHIPID1, R_IAC, R_LADRF = 20, R_PADR, R_MPC = 24,
    R_RNTPC = 26, R_RCVCC, R_UTR = 29, R_RTR1, R_RTR2,
};

#define XMTFC_APADXMT   0x01
#define XMTFS_XMTSV     0x80
#define RCVFC_ASTRPRCV  0x01
#define IR_XMTINT       0x01
#define IR_RCVINT       0x02
#define PR_XMTSV        0x80
#define PR_TDTREQ       0x40
#define PR_RDTREQ       0x20
#define BIUCC_SWRST     0x01
#define MACCC_ENRCV     0x01
#define MACCC_ENXMT     0x02
#define MACCC_DRCVBC    0x04
#define MACCC_DRCVPA    0x08
#define MACCC_PROM      0x80
#define PHYCC_LNKFL     0x80
#define IAC_ADDRCHG     0x80
#define IAC_PHYADDR     0x04
#define IAC_LOGADDR     0x02
#define UTR_LOOP        0x06
#define UTR_RCVFCSE     0x08

#define MACE_ID         0x0940

#define MACE_DEVSTAT_XMTSV  0x20
#define MACE_DEVSTAT_EOF    0x40

static void mace_update_irq(MACEState *s)
{
    qemu_set_irq(s->irq, !!(s->regs[R_IR] & ~s->regs[R_IMR]));
}

static void mace_soft_reset(MACEState *s)
{
    memset(s->regs, 0, sizeof(s->regs));
    s->regs[R_XMTFC] = XMTFC_APADXMT;
    s->regs[R_RCVFC] = RCVFC_ASTRPRCV;
    s->regs[R_FIFOCC] = 0x0f;
    s->addr_ptr = 0;
    s->tx_len = 0;
    s->rx_len = 0;
    s->rx_pos = 0;
    mace_update_irq(s);
}

static bool mace_wanted(MACEState *s, const uint8_t *buf)
{
    unsigned idx;

    if (s->regs[R_MACCC] & MACCC_PROM) {
        return true;
    }
    if (is_broadcast_ether_addr(buf)) {
        return !(s->regs[R_MACCC] & MACCC_DRCVBC);
    }
    if (buf[0] & 1) {
        idx = net_crc32_le(buf, ETH_ALEN) >> 26;
        return s->ladrf[idx >> 3] & (1 << (idx & 7));
    }
    if (s->regs[R_MACCC] & MACCC_DRCVPA) {
        return false;
    }
    return !memcmp(buf, s->padr, 6);
}

/* move FIFO bytes into the armed input command */
static void mace_rx_dma(MACEState *s)
{
    DBDMA_io *io = s->rx_io;
    int n;

    if (!io || !s->rx_len) {
        return;
    }
    n = MIN(s->rx_len - s->rx_pos, io->len);
    trace_mace_rx_dma(io->addr, io->len, io->is_last, n);
    dma_memory_write(&address_space_memory, io->addr, s->rx_frame + s->rx_pos,
                     n, MEMTXATTRS_UNSPECIFIED);
    s->rx_pos += n;
    io->len -= n;
    io->addr += n;
    if (s->rx_pos >= s->rx_len) {
        s->rx_len = 0;
        s->rx_pos = 0;
    }
    if (io->len && s->rx_len) {
        return;
    }
    s->rx_io = NULL;
    /* the channel's device status reports the end of the frame */
    DBDMA_set_devstat(s->dbdma, s->rx_chan, s->rx_len ? 0 : MACE_DEVSTAT_EOF);
    io->dma_end(io);
    if (!s->rx_len) {
        qemu_flush_queued_packets(qemu_get_queue(s->nic));
    }
}

/* a frame off the wire into the receive FIFO */
static void mace_rx_frame(MACEState *s, const uint8_t *buf, size_t size)
{
    uint8_t *f = s->rx_frame;
    size_t len = MIN(size, MACE_FRAME_MAX - 4);
    size_t n;
    uint16_t type;

    memcpy(f, buf, len);
    if (len < 60) {
        memset(f + len, 0, 60 - len);
        len = 60;
    }
    type = lduw_be_p(f + 12);
    if ((s->regs[R_RCVFC] & RCVFC_ASTRPRCV) && type <= 1500 &&
        type + 14 <= len) {
        /* 802.3 frame: pad and FCS stripped */
        len = type + 14;
    } else {
        stl_le_p(f + len, net_crc32_le(f, len) ^ 0xffffffff);
        len += 4;
    }
    /* the FIFO is a halfword wide: the status starts on an even byte */
    n = len + (len & 1);
    f[len] = 0;
    f[n] = len & 0xff;
    f[n + 1] = (len >> 8) & 0x0f;
    f[n + 2] = s->regs[R_RNTPC];
    f[n + 3] = s->regs[R_RCVCC];
    s->rx_len = n + 4;
    s->rx_pos = 0;
    trace_mace_rx(size, s->rx_len);
    s->regs[R_IR] |= IR_RCVINT;
    mace_update_irq(s);
    mace_rx_dma(s);
}

static bool mace_can_receive(NetClientState *nc)
{
    MACEState *s = qemu_get_nic_opaque(nc);

    return (s->regs[R_MACCC] & MACCC_ENRCV) && !s->rx_len;
}

static ssize_t mace_receive(NetClientState *nc, const uint8_t *buf,
                            size_t size)
{
    MACEState *s = qemu_get_nic_opaque(nc);

    if (!(s->regs[R_MACCC] & MACCC_ENRCV) || size < 14 ||
        (s->regs[R_UTR] & UTR_LOOP) > 2 || !mace_wanted(s, buf)) {
        return size;
    }
    if (s->rx_len) {
        return 0;
    }
    mace_rx_frame(s, buf, size);
    return size;
}

static void mace_transmit(MACEState *s)
{
    int len = s->tx_len;

    s->tx_len = 0;
    if (!(s->regs[R_MACCC] & MACCC_ENXMT)) {
        return;
    }
    if (len < 60 && (s->regs[R_XMTFC] & XMTFC_APADXMT)) {
        memset(s->tx_frame + len, 0, 60 - len);
        len = 60;
    }
    trace_mace_tx(len, s->regs[R_UTR] & UTR_LOOP);
    if (s->regs[R_UTR] & UTR_LOOP) {
        if (!s->rx_len && (s->regs[R_MACCC] & MACCC_ENRCV)) {
            mace_rx_frame(s, s->tx_frame, len);
        }
    } else {
        qemu_send_packet(qemu_get_queue(s->nic), s->tx_frame, len);
    }
    s->regs[R_XMTFS] = XMTFS_XMTSV;
    s->regs[R_XMTRC] = 0;
    s->regs[R_PR] |= PR_XMTSV;
    if (s->dbdma) {
        DBDMA_set_devstat(s->dbdma, s->tx_chan, MACE_DEVSTAT_XMTSV);
    }
    s->regs[R_IR] |= IR_XMTINT;
    mace_update_irq(s);
}

static void mace_tx_dma_rw(DBDMA_io *io)
{
    MACEState *s = io->opaque;
    int n;

    trace_mace_tx_dma(io->addr, io->len, io->is_last);
    if (io->is_dma_out) {
        n = MIN(io->len, MACE_FRAME_MAX - s->tx_len);
        dma_memory_read(&address_space_memory, io->addr,
                        s->tx_frame + s->tx_len, n, MEMTXATTRS_UNSPECIFIED);
        s->tx_len += n;
        if (io->is_last) {
            mace_transmit(s);
        }
    }
    io->len = 0;
    io->dma_end(io);
}

static void mace_tx_dma_flush(DBDMA_io *io)
{
}

static void mace_rx_dma_rw(DBDMA_io *io)
{
    MACEState *s = io->opaque;

    trace_mace_rx_dma(io->addr, io->len, io->is_last, -1);
    if (io->is_dma_out) {
        io->len = 0;
        io->dma_end(io);
        return;
    }
    s->rx_io = io;
    if (s->rx_len) {
        mace_rx_dma(s);
    } else {
        qemu_flush_queued_packets(qemu_get_queue(s->nic));
    }
}

static void mace_rx_dma_flush(DBDMA_io *io)
{
    MACEState *s = io->opaque;

    s->rx_io = NULL;
}

void mace_register_dma(MACEState *s, DBDMAState *dbdma, int tx_chan,
                       int rx_chan)
{
    s->dbdma = dbdma;
    s->tx_chan = tx_chan;
    s->rx_chan = rx_chan;
    DBDMA_register_channel(dbdma, tx_chan, s->irq_tx_dma, mace_tx_dma_rw,
                           mace_tx_dma_flush, s);
    DBDMA_register_channel(dbdma, rx_chan, s->irq_rx_dma, mace_rx_dma_rw,
                           mace_rx_dma_flush, s);
}

/*
 * The two address ports share one byte pointer; IAC selects which
 * address it walks (AIX loads the logical filter through PADR).
 */
static bool mace_addr_logical(MACEState *s, unsigned reg)
{
    if (s->regs[R_IAC] & IAC_LOGADDR) {
        return true;
    }
    if (s->regs[R_IAC] & IAC_PHYADDR) {
        return false;
    }
    return reg == R_LADRF;
}

static uint64_t mace_read(void *opaque, hwaddr addr, unsigned size)
{
    MACEState *s = opaque;
    unsigned reg = (addr >> 4) & 0x1f;
    uint8_t val = s->regs[reg];

    switch (reg) {
    case R_RCVFIFO:
        val = 0;
        if (s->rx_len) {
            val = s->rx_frame[s->rx_pos++];
            if (s->rx_pos >= s->rx_len) {
                s->rx_len = 0;
                s->rx_pos = 0;
                qemu_flush_queued_packets(qemu_get_queue(s->nic));
            }
        }
        break;
    case R_IR:
        s->regs[R_IR] = 0;
        mace_update_irq(s);
        break;
    case R_XMTFS:
        s->regs[R_XMTFS] = 0;
        s->regs[R_PR] &= ~PR_XMTSV;
        if (s->dbdma) {
            DBDMA_set_devstat(s->dbdma, s->tx_chan, 0);
        }
        break;
    case R_PR:
        val &= PR_XMTSV;
        if (s->regs[R_MACCC] & MACCC_ENXMT) {
            val |= PR_TDTREQ;
        }
        if (s->rx_len) {
            val |= PR_RDTREQ;
        }
        break;
    case R_FIFOFC:
        val = s->rx_len ? 0x10 : 0;
        break;
    case R_PHYCC:
        val &= ~PHYCC_LNKFL;
        if (qemu_get_queue(s->nic)->link_down) {
            val |= PHYCC_LNKFL;
        }
        break;
    case R_CHIPID0:
        val = MACE_ID & 0xff;
        break;
    case R_CHIPID1:
        val = MACE_ID >> 8;
        break;
    case R_MPC:
    case R_RNTPC:
    case R_RCVCC:
        s->regs[reg] = 0;
        break;
    case R_LADRF:
    case R_PADR:
        if (mace_addr_logical(s, reg)) {
            val = s->ladrf[s->addr_ptr++ & 7];
        } else {
            val = s->padr[s->addr_ptr++ % 6];
        }
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
    case R_XMTFIFO:
        if (s->tx_len < MACE_FRAME_MAX) {
            s->tx_frame[s->tx_len++] = val;
        }
        break;
    case R_BIUCC:
        if (val & BIUCC_SWRST) {
            mace_soft_reset(s);
            break;
        }
        s->regs[reg] = val;
        break;
    case R_IAC:
        s->regs[reg] = val & (IAC_LOGADDR | IAC_PHYADDR);
        if (val & (IAC_LOGADDR | IAC_PHYADDR)) {
            s->addr_ptr = 0;
        }
        break;
    case R_LADRF:
    case R_PADR:
        if (mace_addr_logical(s, reg)) {
            s->ladrf[s->addr_ptr++ & 7] = val;
            if (s->addr_ptr >= 8) {
                s->regs[R_IAC] &= ~IAC_LOGADDR;
                s->addr_ptr = 0;
            }
        } else {
            s->padr[s->addr_ptr++ % 6] = val;
            if (s->addr_ptr >= 6) {
                s->regs[R_IAC] &= ~IAC_PHYADDR;
                s->addr_ptr = 0;
            }
        }
        break;
    case R_IMR:
        s->regs[reg] = val;
        mace_update_irq(s);
        break;
    case R_MACCC:
        s->regs[reg] = val;
        if (val & MACCC_ENRCV) {
            qemu_flush_queued_packets(qemu_get_queue(s->nic));
        }
        break;
    case R_RCVFIFO:
    case R_XMTFS:
    case R_XMTRC:
    case R_RCVFS:
    case R_FIFOFC:
    case R_IR:
    case R_PR:
    case R_CHIPID0:
    case R_CHIPID1:
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
    .valid = {
        .min_access_size = 1,
        .max_access_size = 4,
    },
    .impl = {
        .min_access_size = 1,
        .max_access_size = 1,
    },
};

static NetClientInfo mace_net_info = {
    .type = NET_CLIENT_DRIVER_NIC,
    .size = sizeof(NICState),
    .can_receive = mace_can_receive,
    .receive = mace_receive,
};

static void mace_reset(DeviceState *dev)
{
    MACEState *s = MACE(dev);

    mace_soft_reset(s);
    s->rx_io = NULL;
    memcpy(s->padr, s->conf.macaddr.a, 6);
    memset(s->ladrf, 0, sizeof(s->ladrf));
}

static void mace_realize(DeviceState *dev, Error **errp)
{
    MACEState *s = MACE(dev);

    qemu_macaddr_default_if_unset(&s->conf.macaddr);
    s->nic = qemu_new_nic(&mace_net_info, &s->conf,
                          object_get_typename(OBJECT(dev)), dev->id,
                          &dev->mem_reentrancy_guard, s);
    qemu_format_nic_info_str(qemu_get_queue(s->nic), s->conf.macaddr.a);
}

static void mace_unrealize(DeviceState *dev)
{
    qemu_del_nic(MACE(dev)->nic);
}

static void mace_init(Object *obj)
{
    MACEState *s = MACE(obj);

    memory_region_init_io(&s->mem, obj, &mace_ops, s, "mace", 0x1000);
    sysbus_init_mmio(SYS_BUS_DEVICE(obj), &s->mem);
    sysbus_init_irq(SYS_BUS_DEVICE(obj), &s->irq);
    sysbus_init_irq(SYS_BUS_DEVICE(obj), &s->irq_tx_dma);
    sysbus_init_irq(SYS_BUS_DEVICE(obj), &s->irq_rx_dma);
}

static const VMStateDescription vmstate_mace = {
    .name = "mace",
    .version_id = 2,
    .minimum_version_id = 2,
    .fields = (const VMStateField[]) {
        VMSTATE_UINT8_ARRAY(regs, MACEState, 32),
        VMSTATE_UINT8(addr_ptr, MACEState),
        VMSTATE_UINT8_ARRAY(padr, MACEState, 6),
        VMSTATE_UINT8_ARRAY(ladrf, MACEState, 8),
        VMSTATE_INT32(tx_len, MACEState),
        VMSTATE_UINT8_ARRAY(tx_frame, MACEState, MACE_FRAME_MAX),
        VMSTATE_INT32(rx_len, MACEState),
        VMSTATE_INT32(rx_pos, MACEState),
        VMSTATE_UINT8_ARRAY(rx_frame, MACEState, MACE_FRAME_MAX + 8),
        VMSTATE_END_OF_LIST()
    }
};

static const Property mace_properties[] = {
    DEFINE_NIC_PROPERTIES(MACEState, conf),
};

static void mace_class_init(ObjectClass *oc, const void *data)
{
    DeviceClass *dc = DEVICE_CLASS(oc);

    dc->realize = mace_realize;
    dc->unrealize = mace_unrealize;
    device_class_set_legacy_reset(dc, mace_reset);
    dc->vmsd = &vmstate_mace;
    device_class_set_props(dc, mace_properties);
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
