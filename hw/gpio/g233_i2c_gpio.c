/*
 * QEMU model of the G233 I2C GPIO Controller (Rust experiment)
 *
 * MMIO device at 0x10013000 that acts as an I2C master.
 * Uses the Rust I2C bus library via FFI.
 *
 * SPDX-License-Identifier: GPL-2.0-or-later
 */

#include "qemu/osdep.h"
#include "hw/core/irq.h"
#include "hw/gpio/g233_i2c_gpio.h"
#include "migration/vmstate.h"
#include "qemu/log.h"
#include "qemu/module.h"

/* FFI functions from Rust I2C bus library */
extern void *i2c_bus_create(void);
extern void i2c_bus_destroy(void *bus);
extern void i2c_bus_attach_at24c02(void *bus, uint8_t addr);
extern int i2c_bus_start_transfer(void *bus, uint8_t address, bool is_recv);
extern void i2c_bus_end_transfer(void *bus);
extern int i2c_bus_send(void *bus, uint8_t data);
extern uint8_t i2c_bus_recv(void *bus);

static uint64_t g233_i2c_gpio_read(void *opaque, hwaddr offset, unsigned size)
{
    G233I2cGpioState *s = G233_I2C_GPIO(opaque);

    switch (offset) {
    case G233_I2C_GPIO_CTRL:
        return s->ctrl;
    case G233_I2C_GPIO_STATUS:
        return s->status;
    case G233_I2C_GPIO_ADDR:
        return s->addr;
    case G233_I2C_GPIO_DATA:
        return s->data;
    case G233_I2C_GPIO_PRESCALE:
        return s->prescale;
    default:
        qemu_log_mask(LOG_GUEST_ERROR,
                      "%s: bad read offset 0x%" HWADDR_PRIx "\n",
                      __func__, offset);
        return 0;
    }
}

static void g233_i2c_gpio_write(void *opaque, hwaddr offset,
                                 uint64_t value, unsigned size)
{
    G233I2cGpioState *s = G233_I2C_GPIO(opaque);

    switch (offset) {
    case G233_I2C_GPIO_CTRL:
        s->ctrl = value & 0x0F;

        if (!(value & G233_I2C_GPIO_CTRL_EN)) {
            break;
        }

        s->status = G233_I2C_GPIO_ST_BUSY;
        s->status &= ~G233_I2C_GPIO_ST_DONE;

        if (value & G233_I2C_GPIO_CTRL_START) {
            /* START condition: address the slave */
            bool is_recv = (value & G233_I2C_GPIO_CTRL_RW) != 0;
            if (i2c_bus_start_transfer(s->i2c_bus, (uint8_t)s->addr, is_recv) == 0) {
                s->status |= G233_I2C_GPIO_ST_ACK;
            } else {
                s->status &= ~G233_I2C_GPIO_ST_ACK;
            }
        } else if (value & G233_I2C_GPIO_CTRL_STOP) {
            /* STOP condition */
            i2c_bus_end_transfer(s->i2c_bus);
            s->status &= ~G233_I2C_GPIO_ST_ACK;
        } else {
            /* Data transfer */
            if (value & G233_I2C_GPIO_CTRL_RW) {
                /* Read: receive byte from slave */
                s->data = i2c_bus_recv(s->i2c_bus);
                s->status |= G233_I2C_GPIO_ST_ACK;
            } else {
                /* Write: send byte to slave */
                if (i2c_bus_send(s->i2c_bus, (uint8_t)s->data) == 0) {
                    s->status |= G233_I2C_GPIO_ST_ACK;
                } else {
                    s->status &= ~G233_I2C_GPIO_ST_ACK;
                }
            }
        }

        s->status &= ~G233_I2C_GPIO_ST_BUSY;
        s->status |= G233_I2C_GPIO_ST_DONE;
        break;

    case G233_I2C_GPIO_STATUS:
        /* Read-only from guest */
        break;
    case G233_I2C_GPIO_ADDR:
        s->addr = value & 0x7F;
        break;
    case G233_I2C_GPIO_DATA:
        s->data = value & 0xFF;
        break;
    case G233_I2C_GPIO_PRESCALE:
        s->prescale = value;
        break;
    default:
        qemu_log_mask(LOG_GUEST_ERROR,
                      "%s: bad write offset 0x%" HWADDR_PRIx "\n",
                      __func__, offset);
    }
}

static const MemoryRegionOps g233_i2c_gpio_ops = {
    .read = g233_i2c_gpio_read,
    .write = g233_i2c_gpio_write,
    .endianness = DEVICE_LITTLE_ENDIAN,
    .impl.min_access_size = 4,
    .impl.max_access_size = 4,
};

static void g233_i2c_gpio_reset(DeviceState *dev)
{
    G233I2cGpioState *s = G233_I2C_GPIO(dev);

    s->ctrl = 0;
    s->status = 0;
    s->addr = 0;
    s->data = 0;
    s->prescale = 0;
}

static void g233_i2c_gpio_realize(DeviceState *dev, Error **errp)
{
    G233I2cGpioState *s = G233_I2C_GPIO(dev);

    /* Create Rust I2C bus and attach AT24C02 EEPROM at address 0x50 */
    s->i2c_bus = i2c_bus_create();
    i2c_bus_attach_at24c02(s->i2c_bus, 0x50);

    memory_region_init_io(&s->mmio, OBJECT(dev), &g233_i2c_gpio_ops, s,
                          TYPE_G233_I2C_GPIO, G233_I2C_GPIO_SIZE);
    sysbus_init_mmio(SYS_BUS_DEVICE(dev), &s->mmio);
    sysbus_init_irq(SYS_BUS_DEVICE(dev), &s->irq);
}

static void g233_i2c_gpio_finalize(Object *obj)
{
    G233I2cGpioState *s = G233_I2C_GPIO(obj);

    if (s->i2c_bus) {
        i2c_bus_destroy(s->i2c_bus);
        s->i2c_bus = NULL;
    }
}

static const VMStateDescription vmstate_g233_i2c_gpio = {
    .name = TYPE_G233_I2C_GPIO,
    .version_id = 1,
    .minimum_version_id = 1,
    .fields = (const VMStateField[]) {
        VMSTATE_UINT32(ctrl, G233I2cGpioState),
        VMSTATE_UINT32(status, G233I2cGpioState),
        VMSTATE_UINT32(addr, G233I2cGpioState),
        VMSTATE_UINT32(data, G233I2cGpioState),
        VMSTATE_UINT32(prescale, G233I2cGpioState),
        VMSTATE_END_OF_LIST()
    }
};

static void g233_i2c_gpio_class_init(ObjectClass *klass, const void *data)
{
    DeviceClass *dc = DEVICE_CLASS(klass);

    device_class_set_legacy_reset(dc, g233_i2c_gpio_reset);
    dc->vmsd = &vmstate_g233_i2c_gpio;
    dc->realize = g233_i2c_gpio_realize;
    dc->desc = "G233 I2C GPIO Controller";
}

static const TypeInfo g233_i2c_gpio_info = {
    .name = TYPE_G233_I2C_GPIO,
    .parent = TYPE_SYS_BUS_DEVICE,
    .instance_size = sizeof(G233I2cGpioState),
    .instance_finalize = g233_i2c_gpio_finalize,
    .class_init = g233_i2c_gpio_class_init,
};

static void g233_i2c_gpio_register_types(void)
{
    type_register_static(&g233_i2c_gpio_info);
}

type_init(g233_i2c_gpio_register_types)
