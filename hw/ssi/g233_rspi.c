/*
 * QEMU model of the G233 Rust SPI Controller
 *
 * MMIO device at 0x10019000 with embedded AT25 SPI flash on CS0.
 * Register map: CR1(0x00), SR(0x04), DR(0x08), CS(0x0C)
 *
 * SPDX-License-Identifier: GPL-2.0-or-later
 */

#include "qemu/osdep.h"
#include "hw/core/irq.h"
#include "hw/ssi/g233_rspi.h"
#include "migration/vmstate.h"
#include "qemu/log.h"
#include "qemu/module.h"

/* AT25 SPI flash commands */
#define AT25_CMD_WREN   0x06
#define AT25_CMD_RDSR   0x05
#define AT25_CMD_READ   0x03
#define AT25_CMD_WRITE  0x02

/* AT25 status register bits */
#define AT25_SR_WIP     (1u << 0)
#define AT25_SR_WEL     (1u << 1)

static uint8_t at25_flash_xfer(At25Flash *f, uint8_t tx)
{
    uint8_t rx = 0xFF;

    /*
     * If we are in an active data/address state and a recognized command
     * byte arrives, abort the current operation and redirect to IDLE
     * so the command is processed as a new transaction.
     */
    if (f->state != AT25_STATE_IDLE && f->state != AT25_STATE_RDSR) {
        if (tx == AT25_CMD_WREN || tx == AT25_CMD_RDSR ||
            tx == AT25_CMD_READ || tx == AT25_CMD_WRITE) {
            f->state = AT25_STATE_IDLE;
            f->sr &= ~AT25_SR_WIP;
        }
    }

    switch (f->state) {
    case AT25_STATE_IDLE:
        f->cmd = tx;
        switch (tx) {
        case AT25_CMD_WREN:
            f->wel = true;
            break;
        case AT25_CMD_RDSR:
            f->sr &= ~AT25_SR_WIP;
            f->state = AT25_STATE_RDSR;
            break;
        case AT25_CMD_READ:
            f->state = AT25_STATE_READ_ADDR;
            f->addr = 0;
            f->addr_bytes = 0;
            break;
        case AT25_CMD_WRITE:
            f->state = AT25_STATE_WRITE_ADDR;
            f->addr = 0;
            f->addr_bytes = 0;
            break;
        }
        break;

    case AT25_STATE_RDSR:
        rx = f->sr;
        if (f->wel) {
            rx |= AT25_SR_WEL;
        }
        f->state = AT25_STATE_IDLE;
        break;

    case AT25_STATE_READ_ADDR:
        f->addr = (f->addr << 8) | tx;
        f->addr_bytes++;
        if (f->addr_bytes >= 1) {
            f->state = AT25_STATE_READ_DATA;
        }
        break;

    case AT25_STATE_READ_DATA:
        if (f->addr < AT25_FLASH_SIZE) {
            rx = f->storage[f->addr];
        }
        f->addr = (f->addr + 1) % AT25_FLASH_SIZE;
        break;

    case AT25_STATE_WRITE_ADDR:
        f->addr = (f->addr << 8) | tx;
        f->addr_bytes++;
        if (f->addr_bytes >= 1) {
            f->state = AT25_STATE_WRITE_DATA;
            f->sr |= AT25_SR_WIP;
        }
        break;

    case AT25_STATE_WRITE_DATA:
        if (f->wel && f->addr < AT25_FLASH_SIZE) {
            f->storage[f->addr] = tx;
        }
        f->addr = (f->addr + 1) % AT25_FLASH_SIZE;
        break;
    }

    return rx;
}

static void at25_cs_change(At25Flash *f, bool selected)
{
    if (!selected) {
        /* CS deasserted: end current operation */
        if (f->wel && f->cmd == AT25_CMD_WRITE) {
            f->wel = false;
        }
        f->state = AT25_STATE_IDLE;
        f->cmd = 0;
        f->addr = 0;
        f->addr_bytes = 0;
        f->sr &= ~AT25_SR_WIP;
    }
}

static uint64_t g233_rspi_read(void *opaque, hwaddr offset, unsigned size)
{
    G233RspiState *s = G233_RSPI(opaque);

    switch (offset) {
    case G233_RSPI_CR1:
        return s->cr1;
    case G233_RSPI_SR:
        return s->sr;
    case G233_RSPI_DR:
        s->sr &= ~G233_RSPI_SR_RXNE;
        return s->rx_data;
    case G233_RSPI_CS:
        return s->cs_reg;
    default:
        qemu_log_mask(LOG_GUEST_ERROR,
                      "%s: bad read offset 0x%" HWADDR_PRIx "\n",
                      __func__, offset);
        return 0;
    }
}

static void g233_rspi_write(void *opaque, hwaddr offset,
                             uint64_t value, unsigned size)
{
    G233RspiState *s = G233_RSPI(opaque);

    switch (offset) {
    case G233_RSPI_CR1:
        s->cr1 = value;
        if (s->cr1 & G233_RSPI_CR1_SPE) {
            /* SPI enabled: TX buffer is empty */
            s->sr |= G233_RSPI_SR_TXE;
        } else {
            s->sr = 0;
        }
        break;

    case G233_RSPI_SR:
        /* Read-only */
        break;

    case G233_RSPI_DR:
        if (!(s->cr1 & G233_RSPI_CR1_SPE)) {
            break;
        }

        /* Overrun detection */
        if (s->sr & G233_RSPI_SR_RXNE) {
            s->sr |= G233_RSPI_SR_OVERRUN;
        }

        /* Execute SPI transfer through the flash on CS0 */
        if (s->cs_reg == 0) {
            s->rx_data = at25_flash_xfer(&s->flash, (uint8_t)value);
        } else {
            s->rx_data = 0xFF;
        }

        s->sr |= G233_RSPI_SR_RXNE;
        s->sr |= G233_RSPI_SR_TXE;
        break;

    case G233_RSPI_CS: {
        uint32_t old_cs = s->cs_reg;
        s->cs_reg = value;

        /* CS change: notify flash of deselection */
        if (old_cs != (uint32_t)value) {
            if (old_cs == 0) {
                at25_cs_change(&s->flash, false);
            }
        }
        break;
    }

    default:
        qemu_log_mask(LOG_GUEST_ERROR,
                      "%s: bad write offset 0x%" HWADDR_PRIx "\n",
                      __func__, offset);
    }
}

static const MemoryRegionOps g233_rspi_ops = {
    .read = g233_rspi_read,
    .write = g233_rspi_write,
    .endianness = DEVICE_LITTLE_ENDIAN,
    .impl.min_access_size = 4,
    .impl.max_access_size = 4,
};

static void g233_rspi_reset(DeviceState *dev)
{
    G233RspiState *s = G233_RSPI(dev);

    s->cr1 = 0;
    s->sr = 0;
    s->rx_data = 0;
    s->cs_reg = 0;

    /* Reset flash state */
    memset(&s->flash, 0, sizeof(s->flash));
    memset(s->flash.storage, 0xFF, AT25_FLASH_SIZE);
}

static void g233_rspi_realize(DeviceState *dev, Error **errp)
{
    G233RspiState *s = G233_RSPI(dev);

    /* Initialize flash storage to 0xFF (erased state) */
    memset(s->flash.storage, 0xFF, AT25_FLASH_SIZE);

    memory_region_init_io(&s->mmio, OBJECT(dev), &g233_rspi_ops, s,
                          TYPE_G233_RSPI, G233_RSPI_SIZE);
    sysbus_init_mmio(SYS_BUS_DEVICE(dev), &s->mmio);
    sysbus_init_irq(SYS_BUS_DEVICE(dev), &s->irq);
}

static const VMStateDescription vmstate_g233_rspi = {
    .name = TYPE_G233_RSPI,
    .version_id = 1,
    .minimum_version_id = 1,
    .fields = (const VMStateField[]) {
        VMSTATE_UINT32(cr1, G233RspiState),
        VMSTATE_UINT32(sr, G233RspiState),
        VMSTATE_UINT32(rx_data, G233RspiState),
        VMSTATE_UINT32(cs_reg, G233RspiState),
        VMSTATE_END_OF_LIST()
    }
};

static void g233_rspi_class_init(ObjectClass *klass, const void *data)
{
    DeviceClass *dc = DEVICE_CLASS(klass);

    device_class_set_legacy_reset(dc, g233_rspi_reset);
    dc->vmsd = &vmstate_g233_rspi;
    dc->realize = g233_rspi_realize;
    dc->desc = "G233 Rust SPI Controller";
}

static const TypeInfo g233_rspi_info = {
    .name = TYPE_G233_RSPI,
    .parent = TYPE_SYS_BUS_DEVICE,
    .instance_size = sizeof(G233RspiState),
    .class_init = g233_rspi_class_init,
};

static void g233_rspi_register_types(void)
{
    type_register_static(&g233_rspi_info);
}

type_init(g233_rspi_register_types)
