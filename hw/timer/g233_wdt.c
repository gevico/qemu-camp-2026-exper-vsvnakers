/*
 * QEMU model of the G233 Watchdog Timer
 *
 * SPDX-License-Identifier: GPL-2.0-or-later
 */

#include "qemu/osdep.h"
#include "hw/core/irq.h"
#include "hw/core/qdev-properties.h"
#include "hw/timer/g233_wdt.h"
#include "migration/vmstate.h"
#include "qemu/log.h"
#include "qemu/module.h"

static void g233_wdt_update_irq(G233WdtState *s)
{
    int level = 0;

    if ((s->ctrl & G233_WDT_CTRL_INTEN) &&
        (s->sr & G233_WDT_SR_TIMEOUT)) {
        level = 1;
    }

    qemu_set_irq(s->irq, level);
}

static void g233_wdt_reload(G233WdtState *s)
{
    s->val = s->load;
}

static void g233_wdt_timer_cb(void *opaque)
{
    G233WdtState *s = G233_WDT(opaque);

    if (!(s->ctrl & G233_WDT_CTRL_EN)) {
        return;
    }

    /* Counter reached zero → timeout */
    s->sr |= G233_WDT_SR_TIMEOUT;
    s->val = 0;
    g233_wdt_update_irq(s);
}

static void g233_wdt_restart_timer(G233WdtState *s)
{
    if (!(s->ctrl & G233_WDT_CTRL_EN)) {
        timer_del(&s->timer);
        return;
    }

    int64_t now = qemu_clock_get_ns(QEMU_CLOCK_VIRTUAL);
    timer_mod(&s->timer, now + s->val);
}

static uint64_t g233_wdt_read(void *opaque, hwaddr offset, unsigned size)
{
    G233WdtState *s = G233_WDT(opaque);

    switch (offset) {
    case G233_WDT_CTRL:
        return s->ctrl;
    case G233_WDT_LOAD:
        return s->load;
    case G233_WDT_VAL:
        /* Return current counter value */
        if (s->ctrl & G233_WDT_CTRL_EN) {
            int64_t now = qemu_clock_get_ns(QEMU_CLOCK_VIRTUAL);
            int64_t remaining = timer_expire_time_ns(&s->timer) - now;
            if (remaining < 0) {
                remaining = 0;
            }
            s->val = (uint32_t)remaining;
        }
        return s->val;
    case G233_WDT_SR:
        return s->sr;
    case G233_WDT_KEY:
        return 0;
    default:
        qemu_log_mask(LOG_GUEST_ERROR,
                      "%s: bad read offset 0x%" HWADDR_PRIx "\n",
                      __func__, offset);
        return 0;
    }
}

static void g233_wdt_write(void *opaque, hwaddr offset,
                            uint64_t value, unsigned size)
{
    G233WdtState *s = G233_WDT(opaque);

    switch (offset) {
    case G233_WDT_CTRL:
        if (s->locked) {
            /* When locked, cannot disable the WDT */
            s->ctrl = (s->ctrl & G233_WDT_CTRL_EN) |
                      (value & ~G233_WDT_CTRL_EN);
        } else {
            s->ctrl = value & (G233_WDT_CTRL_EN | G233_WDT_CTRL_INTEN);
        }
        g233_wdt_restart_timer(s);
        g233_wdt_update_irq(s);
        break;
    case G233_WDT_LOAD:
        s->load = value;
        g233_wdt_reload(s);
        g233_wdt_restart_timer(s);
        break;
    case G233_WDT_SR:
        /* Write-1-to-clear */
        s->sr &= ~value;
        g233_wdt_update_irq(s);
        break;
    case G233_WDT_KEY:
        if (value == G233_WDT_KEY_FEED) {
            /* Feed: reload counter */
            g233_wdt_reload(s);
            g233_wdt_restart_timer(s);
        } else if (value == G233_WDT_KEY_LOCK) {
            /* Lock: prevent disabling */
            s->locked = true;
        }
        break;
    default:
        qemu_log_mask(LOG_GUEST_ERROR,
                      "%s: bad write offset 0x%" HWADDR_PRIx "\n",
                      __func__, offset);
    }
}

static const MemoryRegionOps g233_wdt_ops = {
    .read = g233_wdt_read,
    .write = g233_wdt_write,
    .endianness = DEVICE_LITTLE_ENDIAN,
    .impl.min_access_size = 4,
    .impl.max_access_size = 4,
};

static void g233_wdt_reset(DeviceState *dev)
{
    G233WdtState *s = G233_WDT(dev);

    s->ctrl = 0;
    s->load = 0;
    s->val = 0;
    s->sr = 0;
    s->locked = false;
    timer_del(&s->timer);
}

static void g233_wdt_realize(DeviceState *dev, Error **errp)
{
    G233WdtState *s = G233_WDT(dev);

    timer_init_ns(&s->timer, QEMU_CLOCK_VIRTUAL,
                  g233_wdt_timer_cb, s);

    memory_region_init_io(&s->mmio, OBJECT(dev), &g233_wdt_ops, s,
                          TYPE_G233_WDT, G233_WDT_SIZE);
    sysbus_init_mmio(SYS_BUS_DEVICE(dev), &s->mmio);
    sysbus_init_irq(SYS_BUS_DEVICE(dev), &s->irq);
}

static const VMStateDescription vmstate_g233_wdt = {
    .name = TYPE_G233_WDT,
    .version_id = 1,
    .minimum_version_id = 1,
    .fields = (const VMStateField[]) {
        VMSTATE_UINT32(ctrl, G233WdtState),
        VMSTATE_UINT32(load, G233WdtState),
        VMSTATE_UINT32(val, G233WdtState),
        VMSTATE_UINT32(sr, G233WdtState),
        VMSTATE_BOOL(locked, G233WdtState),
        VMSTATE_TIMER(timer, G233WdtState),
        VMSTATE_END_OF_LIST()
    }
};

static void g233_wdt_class_init(ObjectClass *klass, const void *data)
{
    DeviceClass *dc = DEVICE_CLASS(klass);

    device_class_set_legacy_reset(dc, g233_wdt_reset);
    dc->vmsd = &vmstate_g233_wdt;
    dc->realize = g233_wdt_realize;
    dc->desc = "G233 Watchdog Timer";
}

static const TypeInfo g233_wdt_info = {
    .name = TYPE_G233_WDT,
    .parent = TYPE_SYS_BUS_DEVICE,
    .instance_size = sizeof(G233WdtState),
    .class_init = g233_wdt_class_init,
};

static void g233_wdt_register_types(void)
{
    type_register_static(&g233_wdt_info);
}

type_init(g233_wdt_register_types)
