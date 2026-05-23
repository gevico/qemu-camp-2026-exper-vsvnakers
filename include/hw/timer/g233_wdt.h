/*
 * G233 Watchdog Timer header
 *
 * SPDX-License-Identifier: GPL-2.0-or-later
 */

#ifndef HW_TIMER_G233_WDT_H
#define HW_TIMER_G233_WDT_H

#include "hw/core/sysbus.h"
#include "qemu/timer.h"

#define TYPE_G233_WDT "g233-wdt"
OBJECT_DECLARE_SIMPLE_TYPE(G233WdtState, G233_WDT)

#define G233_WDT_SIZE 0x1000

/* Register offsets */
#define G233_WDT_CTRL   0x00
#define G233_WDT_LOAD   0x04
#define G233_WDT_VAL    0x08
#define G233_WDT_SR     0x0C
#define G233_WDT_KEY    0x10

/* CTRL bits */
#define G233_WDT_CTRL_EN     (1u << 0)
#define G233_WDT_CTRL_INTEN  (1u << 1)

/* KEY values */
#define G233_WDT_KEY_FEED    0x5A5A5A5A
#define G233_WDT_KEY_LOCK    0x1ACCE551

/* SR bits */
#define G233_WDT_SR_TIMEOUT  (1u << 0)

struct G233WdtState {
    /*< private >*/
    SysBusDevice parent_obj;

    /*< public >*/
    MemoryRegion mmio;
    QEMUTimer timer;
    qemu_irq irq;

    uint32_t ctrl;
    uint32_t load;
    uint32_t val;
    uint32_t sr;
    bool locked;
};

#endif
