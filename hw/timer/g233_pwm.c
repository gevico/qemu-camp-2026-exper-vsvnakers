/*
 * QEMU model of the G233 PWM Controller
 *
 * SPDX-License-Identifier: GPL-2.0-or-later
 */

#include "qemu/osdep.h"
#include "hw/timer/g233_pwm.h"
#include "migration/vmstate.h"
#include "qemu/log.h"
#include "qemu/module.h"

static void g233_pwm_update_timer(G233PwmState *s, int ch);

static void g233_pwm_handle_timeout(G233PwmState *s, int ch)
{
    if (!(s->ctrl[ch] & G233_PWM_CTRL_EN)) {
        return;
    }
    s->done[ch] = true;
    s->cnt[ch] = 0;
    g233_pwm_update_timer(s, ch);
}

static void g233_pwm_cb0(void *opaque) { g233_pwm_handle_timeout(opaque, 0); }
static void g233_pwm_cb1(void *opaque) { g233_pwm_handle_timeout(opaque, 1); }
static void g233_pwm_cb2(void *opaque) { g233_pwm_handle_timeout(opaque, 2); }
static void g233_pwm_cb3(void *opaque) { g233_pwm_handle_timeout(opaque, 3); }

static void g233_pwm_update_timer(G233PwmState *s, int ch)
{
    if (!(s->ctrl[ch] & G233_PWM_CTRL_EN)) {
        timer_del(&s->timer[ch]);
        return;
    }

    if (s->period[ch] == 0) {
        return;
    }

    int64_t now = qemu_clock_get_ns(QEMU_CLOCK_VIRTUAL);
    int64_t remaining = s->period[ch] - s->cnt[ch];
    if (remaining <= 0) {
        remaining = s->period[ch];
        s->cnt[ch] = 0;
    }
    timer_mod(&s->timer[ch], now + remaining);
}

static uint64_t g233_pwm_read(void *opaque, hwaddr offset, unsigned size)
{
    G233PwmState *s = G233_PWM(opaque);

    if (offset == G233_PWM_GLB) {
        uint32_t val = 0;
        for (int i = 0; i < G233_PWM_CHANS; i++) {
            if (s->ctrl[i] & G233_PWM_CTRL_EN) {
                val |= (1u << i);
            }
            if (s->done[i]) {
                val |= (1u << (4 + i));
            }
        }
        return val;
    }

    for (int i = 0; i < G233_PWM_CHANS; i++) {
        if (offset == G233_PWM_CH_CTRL(i)) {
            return s->ctrl[i];
        }
        if (offset == G233_PWM_CH_PERIOD(i)) {
            return s->period[i];
        }
        if (offset == G233_PWM_CH_DUTY(i)) {
            return s->duty[i];
        }
        if (offset == G233_PWM_CH_CNT(i)) {
            if (s->ctrl[i] & G233_PWM_CTRL_EN) {
                int64_t now = qemu_clock_get_ns(QEMU_CLOCK_VIRTUAL);
                int64_t expire = timer_expire_time_ns(&s->timer[i]);
                if (expire > now) {
                    s->cnt[i] = s->period[i] - (uint32_t)(expire - now);
                } else {
                    s->cnt[i] = 0;
                }
            }
            return s->cnt[i];
        }
    }

    qemu_log_mask(LOG_GUEST_ERROR,
                  "%s: bad read offset 0x%" HWADDR_PRIx "\n",
                  __func__, offset);
    return 0;
}

static void g233_pwm_write(void *opaque, hwaddr offset,
                            uint64_t value, unsigned size)
{
    G233PwmState *s = G233_PWM(opaque);

    if (offset == G233_PWM_GLB) {
        for (int i = 0; i < G233_PWM_CHANS; i++) {
            if (value & (1u << (4 + i))) {
                s->done[i] = false;
            }
        }
        return;
    }

    for (int i = 0; i < G233_PWM_CHANS; i++) {
        if (offset == G233_PWM_CH_CTRL(i)) {
            s->ctrl[i] = value & (G233_PWM_CTRL_EN | G233_PWM_CTRL_POL);
            g233_pwm_update_timer(s, i);
            return;
        }
        if (offset == G233_PWM_CH_PERIOD(i)) {
            s->period[i] = value;
            g233_pwm_update_timer(s, i);
            return;
        }
        if (offset == G233_PWM_CH_DUTY(i)) {
            s->duty[i] = value;
            return;
        }
        if (offset == G233_PWM_CH_CNT(i)) {
            return;
        }
    }

    qemu_log_mask(LOG_GUEST_ERROR,
                  "%s: bad write offset 0x%" HWADDR_PRIx "\n",
                  __func__, offset);
}

static const MemoryRegionOps g233_pwm_ops = {
    .read = g233_pwm_read,
    .write = g233_pwm_write,
    .endianness = DEVICE_LITTLE_ENDIAN,
    .impl.min_access_size = 4,
    .impl.max_access_size = 4,
};

static void g233_pwm_reset(DeviceState *dev)
{
    G233PwmState *s = G233_PWM(dev);

    for (int i = 0; i < G233_PWM_CHANS; i++) {
        s->ctrl[i] = 0;
        s->period[i] = 0;
        s->duty[i] = 0;
        s->cnt[i] = 0;
        s->done[i] = false;
        timer_del(&s->timer[i]);
    }
}

static void g233_pwm_realize(DeviceState *dev, Error **errp)
{
    G233PwmState *s = G233_PWM(dev);
    static void (*const cbs[])(void *) = {
        g233_pwm_cb0, g233_pwm_cb1, g233_pwm_cb2, g233_pwm_cb3,
    };

    for (int i = 0; i < G233_PWM_CHANS; i++) {
        timer_init_ns(&s->timer[i], QEMU_CLOCK_VIRTUAL, cbs[i], s);
    }

    memory_region_init_io(&s->mmio, OBJECT(dev), &g233_pwm_ops, s,
                          TYPE_G233_PWM, G233_PWM_SIZE);
    sysbus_init_mmio(SYS_BUS_DEVICE(dev), &s->mmio);
}

static const VMStateDescription vmstate_g233_pwm = {
    .name = TYPE_G233_PWM,
    .version_id = 1,
    .minimum_version_id = 1,
    .fields = (const VMStateField[]) {
        VMSTATE_UINT32_ARRAY(ctrl, G233PwmState, G233_PWM_CHANS),
        VMSTATE_UINT32_ARRAY(period, G233PwmState, G233_PWM_CHANS),
        VMSTATE_UINT32_ARRAY(duty, G233PwmState, G233_PWM_CHANS),
        VMSTATE_UINT32_ARRAY(cnt, G233PwmState, G233_PWM_CHANS),
        VMSTATE_BOOL_ARRAY(done, G233PwmState, G233_PWM_CHANS),
        VMSTATE_TIMER_ARRAY(timer, G233PwmState, G233_PWM_CHANS),
        VMSTATE_END_OF_LIST()
    }
};

static void g233_pwm_class_init(ObjectClass *klass, const void *data)
{
    DeviceClass *dc = DEVICE_CLASS(klass);

    device_class_set_legacy_reset(dc, g233_pwm_reset);
    dc->vmsd = &vmstate_g233_pwm;
    dc->realize = g233_pwm_realize;
    dc->desc = "G233 PWM";
}

static const TypeInfo g233_pwm_info = {
    .name = TYPE_G233_PWM,
    .parent = TYPE_SYS_BUS_DEVICE,
    .instance_size = sizeof(G233PwmState),
    .class_init = g233_pwm_class_init,
};

static void g233_pwm_register_types(void)
{
    type_register_static(&g233_pwm_info);
}

type_init(g233_pwm_register_types)
