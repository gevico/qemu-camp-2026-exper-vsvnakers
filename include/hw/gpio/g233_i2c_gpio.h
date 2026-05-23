/*
 * G233 I2C GPIO Controller header (Rust experiment)
 *
 * SPDX-License-Identifier: GPL-2.0-or-later
 */

#ifndef HW_GPIO_G233_I2C_GPIO_H
#define HW_GPIO_G233_I2C_GPIO_H

#include "hw/core/sysbus.h"

#define TYPE_G233_I2C_GPIO "g233-i2c-gpio"
OBJECT_DECLARE_SIMPLE_TYPE(G233I2cGpioState, G233_I2C_GPIO)

#define G233_I2C_GPIO_SIZE 0x1000

/* Register offsets */
#define G233_I2C_GPIO_CTRL      0x00
#define G233_I2C_GPIO_STATUS    0x04
#define G233_I2C_GPIO_ADDR      0x08
#define G233_I2C_GPIO_DATA      0x0C
#define G233_I2C_GPIO_PRESCALE  0x10

/* CTRL bits */
#define G233_I2C_GPIO_CTRL_EN       (1u << 0)
#define G233_I2C_GPIO_CTRL_START    (1u << 1)
#define G233_I2C_GPIO_CTRL_STOP     (1u << 2)
#define G233_I2C_GPIO_CTRL_RW       (1u << 3)

/* STATUS bits */
#define G233_I2C_GPIO_ST_BUSY   (1u << 0)
#define G233_I2C_GPIO_ST_ACK    (1u << 1)
#define G233_I2C_GPIO_ST_DONE   (1u << 2)

struct G233I2cGpioState {
    /*< private >*/
    SysBusDevice parent_obj;

    /*< public >*/
    MemoryRegion mmio;
    qemu_irq irq;

    /* Registers */
    uint32_t ctrl;
    uint32_t status;
    uint32_t addr;
    uint32_t data;
    uint32_t prescale;

    /* I2C bus (managed from Rust) */
    void *i2c_bus;
};

#endif
