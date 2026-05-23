/*
 * QEMU model of the G233 GPIO Controller
 *
 * SPDX-License-Identifier: GPL-2.0-or-later
 */

#include "qemu/osdep.h"
#include "hw/core/irq.h"
#include "hw/gpio/g233_gpio.h"
#include "migration/vmstate.h"
#include "qemu/log.h"
#include "qemu/module.h"

/*
 * Recompute the IN register: for output pins, IN reflects OUT.
 */
static void g233_gpio_update_in(G233GpioState *s)
{
    /* For pins configured as output, IN = OUT */
    s->in = (s->out & s->dir);
    /* For input-only pins, IN retains external state (not modeled) */
}

static void g233_gpio_update_irq(G233GpioState *s)
{
    /* Level-triggered pins: IS reflects current pin state in real time */
    for (int i = 0; i < 32; i++) {
        if (!(s->ie & (1u << i))) {
            continue;
        }
        if (!(s->trig & (1u << i))) {
            continue; /* edge-triggered: IS set by edge detection */
        }

        uint32_t pin_val = (s->in >> i) & 1;
        if (s->pol & (1u << i)) {
            /* High level */
            if (pin_val) {
                s->is |= (1u << i);
            } else {
                s->is &= ~(1u << i);
            }
        } else {
            /* Low level */
            if (!pin_val) {
                s->is |= (1u << i);
            } else {
                s->is &= ~(1u << i);
            }
        }
    }

    qemu_set_irq(s->irq, (s->is != 0) ? 1 : 0);
}

static uint64_t g233_gpio_read(void *opaque, hwaddr offset, unsigned size)
{
    G233GpioState *s = G233_GPIO(opaque);

    switch (offset) {
    case G233_GPIO_DIR:
        return s->dir;
    case G233_GPIO_OUT:
        return s->out;
    case G233_GPIO_IN:
        g233_gpio_update_in(s);
        return s->in;
    case G233_GPIO_IE:
        return s->ie;
    case G233_GPIO_IS:
        return s->is;
    case G233_GPIO_TRIG:
        return s->trig;
    case G233_GPIO_POL:
        return s->pol;
    default:
        qemu_log_mask(LOG_GUEST_ERROR,
                      "%s: bad read offset 0x%" HWADDR_PRIx "\n",
                      __func__, offset);
        return 0;
    }
}

static void g233_gpio_write(void *opaque, hwaddr offset,
                             uint64_t value, unsigned size)
{
    G233GpioState *s = G233_GPIO(opaque);

    switch (offset) {
    case G233_GPIO_DIR:
        s->dir = value;
        g233_gpio_update_in(s);
        g233_gpio_update_irq(s);
        break;
    case G233_GPIO_OUT: {
        uint32_t old_in = s->in;
        s->out = value;
        g233_gpio_update_in(s);

        /* Check for edge transitions on output pins */
        for (int i = 0; i < 32; i++) {
            if (!(s->dir & (1u << i))) {
                continue; /* not an output pin */
            }
            if (!(s->ie & (1u << i))) {
                continue; /* interrupt not enabled */
            }
            if (s->trig & (1u << i)) {
                continue; /* level-triggered, handled by update_irq */
            }

            uint32_t old_bit = (old_in >> i) & 1;
            uint32_t new_bit = (s->in >> i) & 1;
            bool triggered = false;

            if (s->pol & (1u << i)) {
                /* Rising edge */
                triggered = (!old_bit && new_bit);
            } else {
                /* Falling edge */
                triggered = (old_bit && !new_bit);
            }

            if (triggered) {
                s->is |= (1u << i);
            }
        }

        g233_gpio_update_irq(s);
        break;
    }
    case G233_GPIO_IE:
        s->ie = value;
        g233_gpio_update_irq(s);
        break;
    case G233_GPIO_IS:
        /* Write-1-to-clear */
        s->is &= ~value;
        g233_gpio_update_irq(s);
        break;
    case G233_GPIO_TRIG:
        s->trig = value;
        g233_gpio_update_irq(s);
        break;
    case G233_GPIO_POL:
        s->pol = value;
        g233_gpio_update_irq(s);
        break;
    default:
        qemu_log_mask(LOG_GUEST_ERROR,
                      "%s: bad write offset 0x%" HWADDR_PRIx "\n",
                      __func__, offset);
    }
}

static const MemoryRegionOps g233_gpio_ops = {
    .read = g233_gpio_read,
    .write = g233_gpio_write,
    .endianness = DEVICE_LITTLE_ENDIAN,
    .impl.min_access_size = 4,
    .impl.max_access_size = 4,
};

static void g233_gpio_reset(DeviceState *dev)
{
    G233GpioState *s = G233_GPIO(dev);

    s->dir = 0;
    s->out = 0;
    s->in = 0;
    s->ie = 0;
    s->is = 0;
    s->trig = 0;
    s->pol = 0;
}

static void g233_gpio_realize(DeviceState *dev, Error **errp)
{
    G233GpioState *s = G233_GPIO(dev);

    memory_region_init_io(&s->mmio, OBJECT(dev), &g233_gpio_ops, s,
                          TYPE_G233_GPIO, G233_GPIO_SIZE);
    sysbus_init_mmio(SYS_BUS_DEVICE(dev), &s->mmio);
    sysbus_init_irq(SYS_BUS_DEVICE(dev), &s->irq);
}

static const VMStateDescription vmstate_g233_gpio = {
    .name = TYPE_G233_GPIO,
    .version_id = 1,
    .minimum_version_id = 1,
    .fields = (const VMStateField[]) {
        VMSTATE_UINT32(dir, G233GpioState),
        VMSTATE_UINT32(out, G233GpioState),
        VMSTATE_UINT32(in, G233GpioState),
        VMSTATE_UINT32(ie, G233GpioState),
        VMSTATE_UINT32(is, G233GpioState),
        VMSTATE_UINT32(trig, G233GpioState),
        VMSTATE_UINT32(pol, G233GpioState),
        VMSTATE_END_OF_LIST()
    }
};

static void g233_gpio_class_init(ObjectClass *klass, const void *data)
{
    DeviceClass *dc = DEVICE_CLASS(klass);

    device_class_set_legacy_reset(dc, g233_gpio_reset);
    dc->vmsd = &vmstate_g233_gpio;
    dc->realize = g233_gpio_realize;
    dc->desc = "G233 GPIO";
}

static const TypeInfo g233_gpio_info = {
    .name = TYPE_G233_GPIO,
    .parent = TYPE_SYS_BUS_DEVICE,
    .instance_size = sizeof(G233GpioState),
    .class_init = g233_gpio_class_init,
};

static void g233_gpio_register_types(void)
{
    type_register_static(&g233_gpio_info);
}

type_init(g233_gpio_register_types)
