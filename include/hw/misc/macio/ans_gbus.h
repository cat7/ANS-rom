/*
 * Apple Network Server GBus devices behind Grand Central
 *
 * SPDX-License-Identifier: GPL-2.0-or-later
 */

#ifndef HW_MISC_MACIO_ANS_GBUS_H
#define HW_MISC_MACIO_ANS_GBUS_H

#include "hw/core/sysbus.h"
#include "chardev/char-fe.h"
#include "net/net.h"
#include "qom/object.h"

#define TYPE_ANS_GBUS "ans-gbus"
OBJECT_DECLARE_SIMPLE_TYPE(ANSGBusState, ANS_GBUS)

/* the window starts at Grand Central +0x19000 */
#define ANS_GBUS_OFFSET     0x19000
#define ANS_GBUS_SIZE       0x7000

#define ANS_NVRAM_SIZE      0x2000
#define ANS_LCD_LINES       4
#define ANS_LCD_COLS        20
#define ANS_LCD_DDRAM       128

struct ANSGBusState {
    SysBusDevice parent_obj;

    MemoryRegion mem;
    CharFrontend lcd_chr;
    QEMUTimer *lcd_timer;

    /* board */
    char *keyswitch;
    bool two_supplies;
    uint32_t slots;
    uint16_t env_faults;
    uint16_t tb_enable;
    uint16_t misc;
    uint32_t doorbell;
    MACAddr macaddr;
    uint8_t eprom[16];

    /* LCD (HD44780, 4 x 20) */
    uint8_t ddram[ANS_LCD_DDRAM];
    uint8_t lcd_addr;
    bool lcd_cgram;
    bool lcd_on;
    bool lcd_inc;
    uint8_t lcd_shown[ANS_LCD_LINES][ANS_LCD_COLS];

    /* NVRAM */
    char *nvram_file;
    int nvram_fd;
    uint8_t nvram_bank;
    uint8_t nvram[ANS_NVRAM_SIZE];
};

#endif
