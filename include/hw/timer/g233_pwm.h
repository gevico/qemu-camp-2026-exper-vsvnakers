/*
 * G233 PWM Controller header
 *
 * SPDX-License-Identifier: GPL-2.0-or-later
 */

#ifndef HW_TIMER_G233_PWM_H
#define HW_TIMER_G233_PWM_H

#include "hw/core/sysbus.h"
#include "qemu/timer.h"

#define TYPE_G233_PWM "g233-pwm"
OBJECT_DECLARE_SIMPLE_TYPE(G233PwmState, G233_PWM)

#define G233_PWM_SIZE    0x1000
#define G233_PWM_CHANS   4

/* Global register */
#define G233_PWM_GLB     0x00

/* Per-channel registers: CHn at 0x10 + n*0x10 */
#define G233_PWM_CH_BASE 0x10
#define G233_PWM_CH_STRIDE 0x10
#define G233_PWM_CH_CTRL(n)   (G233_PWM_CH_BASE + (n) * G233_PWM_CH_STRIDE + 0x00)
#define G233_PWM_CH_PERIOD(n) (G233_PWM_CH_BASE + (n) * G233_PWM_CH_STRIDE + 0x04)
#define G233_PWM_CH_DUTY(n)   (G233_PWM_CH_BASE + (n) * G233_PWM_CH_STRIDE + 0x08)
#define G233_PWM_CH_CNT(n)    (G233_PWM_CH_BASE + (n) * G233_PWM_CH_STRIDE + 0x0C)

/* CHn_CTRL bits */
#define G233_PWM_CTRL_EN   (1u << 0)
#define G233_PWM_CTRL_POL  (1u << 1)

struct G233PwmState {
    /*< private >*/
    SysBusDevice parent_obj;

    /*< public >*/
    MemoryRegion mmio;
    QEMUTimer timer[G233_PWM_CHANS];

    uint32_t ctrl[G233_PWM_CHANS];
    uint32_t period[G233_PWM_CHANS];
    uint32_t duty[G233_PWM_CHANS];
    uint32_t cnt[G233_PWM_CHANS];
    bool done[G233_PWM_CHANS];
};

#endif
