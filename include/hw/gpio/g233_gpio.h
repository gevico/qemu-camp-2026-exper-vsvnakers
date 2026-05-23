/*
 * G233 GPIO Controller header
 *
 * SPDX-License-Identifier: GPL-2.0-or-later
 */

#ifndef HW_GPIO_G233_GPIO_H
#define HW_GPIO_G233_GPIO_H

#include "hw/core/sysbus.h"

#define TYPE_G233_GPIO "g233-gpio"
OBJECT_DECLARE_SIMPLE_TYPE(G233GpioState, G233_GPIO)

#define G233_GPIO_SIZE 0x1000

/* Register offsets */
#define G233_GPIO_DIR    0x00
#define G233_GPIO_OUT    0x04
#define G233_GPIO_IN     0x08
#define G233_GPIO_IE     0x0C
#define G233_GPIO_IS     0x10
#define G233_GPIO_TRIG   0x14
#define G233_GPIO_POL    0x18

struct G233GpioState {
    /*< private >*/
    SysBusDevice parent_obj;

    /*< public >*/
    MemoryRegion mmio;
    qemu_irq irq;

    uint32_t dir;
    uint32_t out;
    uint32_t in;
    uint32_t ie;
    uint32_t is;
    uint32_t trig;
    uint32_t pol;
};

#endif
