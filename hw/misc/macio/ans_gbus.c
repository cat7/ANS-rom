/*
 * Apple Network Server GBus devices behind Grand Central
 *
 * Register behaviour after the Granny Smith emulator's model (MIT,
 * Copyright (c) pappadf).
 *
 * SPDX-License-Identifier: GPL-2.0-or-later
 *
 * Grand Central's generic bus chip selects carry, on the Network Server:
 *   +0x19000  Ethernet address PROM, 16 byte cells on 0x10 centres
 *             (any access is also the SecToPri doorbell)
 *   +0x1A000  Board Register 1, little-endian (Macintosh BoxID)
 *   +0x1C000  front-panel LCD: command +0x00, data +0x10 (write only),
 *             timebase enable +0x20, +0x30 write-and-readback
 *   +0x1D000  NVRAM bank select (bank = address / 32)
 *   +0x1E000  Board Register 2, environment, little-endian, active low
 *   +0x1F000  NVRAM data: 32 byte cells on 0x10 centres, wrapping
 *             every 0x200
 *
 * Every Board Register bit is active low unless its name ends in H.
 */

#include "qemu/osdep.h"
#include "qapi/error.h"
#include "qemu/error-report.h"
#include "qemu/log.h"
#include "qemu/module.h"
#include "qemu/timer.h"
#include "hw/core/qdev-properties.h"
#include "hw/core/qdev-properties-system.h"
#include "hw/misc/macio/ans_gbus.h"
#include "migration/vmstate.h"
#include "trace.h"

#define GB_EPROM    0x0000
#define GB_BREG1    0x1000
#define GB_LCD      0x3000
#define GB_NVPORT   0x4000
#define GB_BREG2    0x5000
#define GB_NVDATA   0x6000

#define LCD_CMD     0x00
#define LCD_DATA    0x10
#define LCD_TBEN    0x20
#define LCD_MISC    0x30

/* Board Register 1 */
#define BREG1_STRAPS        0x0900  /* BoxId0 = 1, BoxId1 = 0, bit 8 high */
#define BREG1_SERVICE_L     0x2000
#define BREG1_LOCKED_L      0x4000
#define BREG1_TWO_PSU_H     0x8000

static const uint8_t lcd_line_base[ANS_LCD_LINES] = { 0x00, 0x40, 0x14, 0x54 };

static uint32_t gbus_breg1(ANSGBusState *s)
{
    uint32_t v = BREG1_STRAPS | (s->slots & 0x3f);

    if (!strcmp(s->keyswitch, "locked")) {
        v |= BREG1_SERVICE_L;
    } else if (!strcmp(s->keyswitch, "service")) {
        v |= BREG1_LOCKED_L;
    } else {
        v |= BREG1_SERVICE_L | BREG1_LOCKED_L;
    }
    if (s->two_supplies) {
        v |= BREG1_TWO_PSU_H;
    }
    return v;
}

static uint16_t gbus_breg2(ANSGBusState *s)
{
    return 0xffff & ~s->env_faults;
}

/* LCD */

static void lcd_flush(ANSGBusState *s)
{
    int line, i;

    for (line = 0; line < ANS_LCD_LINES; line++) {
        const uint8_t *src = s->ddram + lcd_line_base[line];
        char text[ANS_LCD_COLS + 1];
        char out[ANS_LCD_COLS + 16];
        int n;

        if (!memcmp(s->lcd_shown[line], src, ANS_LCD_COLS)) {
            continue;
        }
        memcpy(s->lcd_shown[line], src, ANS_LCD_COLS);
        for (i = 0; i < ANS_LCD_COLS; i++) {
            text[i] = (src[i] >= 0x20 && src[i] < 0x7f) ? src[i] : ' ';
        }
        text[ANS_LCD_COLS] = 0;
        trace_ans_lcd_line(line, text);
        if (qemu_chr_fe_backend_connected(&s->lcd_chr)) {
            n = snprintf(out, sizeof(out), "lcd%d: %s\n", line, text);
            qemu_chr_fe_write_all(&s->lcd_chr, (uint8_t *)out, n);
        }
    }
}

static void lcd_timer_cb(void *opaque)
{
    lcd_flush(opaque);
}

static void lcd_command(ANSGBusState *s, uint8_t cmd)
{
    trace_ans_lcd_command(cmd);
    if (cmd & 0x80) {
        s->lcd_addr = cmd & 0x7f;
        s->lcd_cgram = false;
    } else if (cmd & 0x40) {
        s->lcd_addr = cmd & 0x3f;
        s->lcd_cgram = true;
    } else if (cmd & 0x20) {
        /* function set */
    } else if (cmd & 0x10) {
        qemu_log_mask(LOG_UNIMP, "ans-lcd: cursor/display shift 0x%02x\n",
                      cmd);
    } else if (cmd & 0x08) {
        s->lcd_on = (cmd & 0x04) != 0;
    } else if (cmd & 0x04) {
        s->lcd_inc = (cmd & 0x02) != 0;
    } else if (cmd & 0x02) {
        s->lcd_addr = 0;
        s->lcd_cgram = false;
    } else if (cmd & 0x01) {
        memset(s->ddram, ' ', sizeof(s->ddram));
        s->lcd_addr = 0;
        s->lcd_cgram = false;
    }
    lcd_flush(s);
}

static void lcd_data(ANSGBusState *s, uint8_t ch)
{
    if (!s->lcd_cgram) {
        s->ddram[s->lcd_addr & (ANS_LCD_DDRAM - 1)] = ch;
    }
    s->lcd_addr = (s->lcd_addr + (s->lcd_inc ? 1 : -1)) & (ANS_LCD_DDRAM - 1);
    timer_mod(s->lcd_timer,
              qemu_clock_get_ms(QEMU_CLOCK_VIRTUAL) + 50);
}

/* NVRAM */

static unsigned nvram_index(ANSGBusState *s, hwaddr off)
{
    return (s->nvram_bank * 32 + ((off >> 4) & 0x1f)) % ANS_NVRAM_SIZE;
}

static void nvram_store(ANSGBusState *s, unsigned idx, uint8_t val)
{
    s->nvram[idx] = val;
    if (s->nvram_fd >= 0 &&
        pwrite(s->nvram_fd, &val, 1, idx) != 1) {
        warn_report_once("ans-gbus: NVRAM file write failed");
    }
}

/* EPROM */

static void eprom_fill(ANSGBusState *s)
{
    uint8_t x = 0;
    int i;

    for (i = 0; i < 6; i++) {
        s->eprom[i] = s->macaddr.a[i];
        s->eprom[8 + i] = ~s->macaddr.a[i];
        x ^= s->macaddr.a[i];
    }
    s->eprom[6] = 0xaa;
    s->eprom[7] = ~x;
    s->eprom[14] = 0x55;
    s->eprom[15] = ~x;
}

static uint8_t gbus_read_byte(ANSGBusState *s, hwaddr addr, bool wide)
{
    hwaddr off = addr & 0xfff;

    switch (addr & 0x7000) {
    case GB_EPROM:
        s->doorbell++;
        if ((off & 0xf) || (off >> 4) >= 16) {
            return wide ? 0 : 0xff;
        }
        return s->eprom[off >> 4];
    case GB_BREG1:
        return gbus_breg1(s) >> (8 * (off & 3));
    case GB_BREG2:
        return gbus_breg2(s) >> (8 * (off & 1));
    case GB_LCD:
        switch (off & 0x30) {
        case LCD_TBEN:
            return s->tb_enable >> (off & 1 ? 0 : 8);
        case LCD_MISC:
            return s->misc >> (off & 1 ? 0 : 8);
        }
        return 0;
    case GB_NVPORT:
        return s->nvram_bank;
    case GB_NVDATA:
        if (off & 0xf) {
            return wide ? 0 : 0xff;
        }
        return s->nvram[nvram_index(s, off)];
    }
    qemu_log_mask(LOG_UNIMP, "ans-gbus: read of +0x%05" HWADDR_PRIx "\n",
                  addr + ANS_GBUS_OFFSET);
    return 0;
}

static void gbus_write_byte(ANSGBusState *s, hwaddr addr, uint8_t val)
{
    hwaddr off = addr & 0xfff;

    switch (addr & 0x7000) {
    case GB_EPROM:
        s->doorbell++;
        return;
    case GB_LCD:
        switch (off & 0x30) {
        case LCD_CMD:
            if (!(off & 0xf)) {
                lcd_command(s, val);
            }
            return;
        case LCD_DATA:
            if (!(off & 0xf)) {
                lcd_data(s, val);
            }
            return;
        case LCD_TBEN:
            s->tb_enable = off & 1 ? (s->tb_enable & 0xff00) | val
                                   : (s->tb_enable & 0x00ff) | (val << 8);
            return;
        case LCD_MISC:
            s->misc = off & 1 ? (s->misc & 0xff00) | val
                              : (s->misc & 0x00ff) | (val << 8);
            return;
        }
        return;
    case GB_NVPORT:
        if (!(off & 0xf)) {
            s->nvram_bank = val;
        }
        return;
    case GB_NVDATA:
        if (!(off & 0xf)) {
            nvram_store(s, nvram_index(s, off), val);
        }
        return;
    case GB_BREG1:
    case GB_BREG2:
        return;
    }
    qemu_log_mask(LOG_UNIMP, "ans-gbus: write of +0x%05" HWADDR_PRIx
                  " = 0x%02x\n", addr + ANS_GBUS_OFFSET, val);
}

static uint64_t gbus_read(void *opaque, hwaddr addr, unsigned size)
{
    ANSGBusState *s = opaque;
    uint64_t val = 0;
    unsigned i;

    for (i = 0; i < size; i++) {
        val = (val << 8) | gbus_read_byte(s, addr + i, size > 1);
    }
    trace_ans_gbus_read(addr + ANS_GBUS_OFFSET, size, val);
    return val;
}

static void gbus_write(void *opaque, hwaddr addr, uint64_t val,
                       unsigned size)
{
    ANSGBusState *s = opaque;
    unsigned i;

    trace_ans_gbus_write(addr + ANS_GBUS_OFFSET, size, val);
    for (i = 0; i < size; i++) {
        gbus_write_byte(s, addr + i, val >> (8 * (size - 1 - i)));
    }
}

static const MemoryRegionOps gbus_ops = {
    .read = gbus_read,
    .write = gbus_write,
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

static void gbus_reset(DeviceState *dev)
{
    ANSGBusState *s = ANS_GBUS(dev);

    s->tb_enable = 0x8000;
    s->misc = 0;
    s->doorbell = 0;
    s->nvram_bank = 0;
    memset(s->ddram, ' ', sizeof(s->ddram));
    memset(s->lcd_shown, ' ', sizeof(s->lcd_shown));
    s->lcd_addr = 0;
    s->lcd_cgram = false;
    s->lcd_on = false;
    s->lcd_inc = true;
    eprom_fill(s);
}

static void gbus_realize(DeviceState *dev, Error **errp)
{
    ANSGBusState *s = ANS_GBUS(dev);

    if (strcmp(s->keyswitch, "normal") && strcmp(s->keyswitch, "locked") &&
        strcmp(s->keyswitch, "service")) {
        error_setg(errp, "keyswitch must be normal, locked or service");
        return;
    }

    s->nvram_fd = -1;
    memset(s->nvram, 0, sizeof(s->nvram));
    if (s->nvram_file && *s->nvram_file) {
        s->nvram_fd = open(s->nvram_file, O_RDWR | O_CREAT | O_BINARY, 0644);
        if (s->nvram_fd < 0) {
            error_setg_errno(errp, errno, "cannot open NVRAM file '%s'",
                             s->nvram_file);
            return;
        }
        if (pread(s->nvram_fd, s->nvram, ANS_NVRAM_SIZE, 0) < 0) {
            error_setg_errno(errp, errno, "cannot read NVRAM file '%s'",
                             s->nvram_file);
            return;
        }
        if (ftruncate(s->nvram_fd, ANS_NVRAM_SIZE) < 0) {
            warn_report("ans-gbus: cannot size NVRAM file '%s'",
                        s->nvram_file);
        }
    }
    s->lcd_timer = timer_new_ms(QEMU_CLOCK_VIRTUAL, lcd_timer_cb, s);
}

static void gbus_init(Object *obj)
{
    ANSGBusState *s = ANS_GBUS(obj);

    memory_region_init_io(&s->mem, obj, &gbus_ops, s, "ans-gbus",
                          ANS_GBUS_SIZE);
    sysbus_init_mmio(SYS_BUS_DEVICE(obj), &s->mem);
}

static const VMStateDescription vmstate_ans_gbus = {
    .name = "ans-gbus",
    .version_id = 1,
    .minimum_version_id = 1,
    .fields = (const VMStateField[]) {
        VMSTATE_UINT16(tb_enable, ANSGBusState),
        VMSTATE_UINT16(misc, ANSGBusState),
        VMSTATE_UINT8_ARRAY(ddram, ANSGBusState, ANS_LCD_DDRAM),
        VMSTATE_UINT8(lcd_addr, ANSGBusState),
        VMSTATE_BOOL(lcd_cgram, ANSGBusState),
        VMSTATE_BOOL(lcd_on, ANSGBusState),
        VMSTATE_BOOL(lcd_inc, ANSGBusState),
        VMSTATE_UINT8(nvram_bank, ANSGBusState),
        VMSTATE_UINT8_ARRAY(nvram, ANSGBusState, ANS_NVRAM_SIZE),
        VMSTATE_END_OF_LIST()
    }
};

static const Property gbus_properties[] = {
    DEFINE_PROP_STRING("keyswitch", ANSGBusState, keyswitch),
    DEFINE_PROP_BOOL("two-supplies", ANSGBusState, two_supplies, false),
    DEFINE_PROP_UINT32("slots", ANSGBusState, slots, 0),
    DEFINE_PROP_UINT16("env-faults", ANSGBusState, env_faults, 0),
    DEFINE_PROP_MACADDR("mac", ANSGBusState, macaddr),
    DEFINE_PROP_STRING("nvram-file", ANSGBusState, nvram_file),
    DEFINE_PROP_CHR("lcd", ANSGBusState, lcd_chr),
};

static void gbus_instance_post_init(Object *obj)
{
    ANSGBusState *s = ANS_GBUS(obj);

    if (!s->keyswitch) {
        s->keyswitch = g_strdup("normal");
    }
}

static void gbus_class_init(ObjectClass *oc, const void *data)
{
    DeviceClass *dc = DEVICE_CLASS(oc);

    dc->realize = gbus_realize;
    device_class_set_legacy_reset(dc, gbus_reset);
    dc->vmsd = &vmstate_ans_gbus;
    device_class_set_props(dc, gbus_properties);
}

static const TypeInfo gbus_info = {
    .name          = TYPE_ANS_GBUS,
    .parent        = TYPE_SYS_BUS_DEVICE,
    .instance_size = sizeof(ANSGBusState),
    .instance_init = gbus_init,
    .instance_post_init = gbus_instance_post_init,
    .class_init    = gbus_class_init,
};

static void gbus_register_types(void)
{
    type_register_static(&gbus_info);
}

type_init(gbus_register_types)
