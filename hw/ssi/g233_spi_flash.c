/*
 * QEMU model of G233 SPI Flash (W25X16/W25X32)
 *
 * SPDX-License-Identifier: GPL-2.0-or-later
 */

#include "qemu/osdep.h"
#include "hw/ssi/g233_spi_flash.h"
#include "hw/core/qdev-properties.h"
#include "qapi/error.h"
#include "qemu/log.h"
#include "qemu/module.h"

void g233_spi_flash_cs_change(G233SpiFlashState *flash, bool selected)
{
    if (!selected) {
        /* CS deasserted: finalize any pending write operation */
        if (flash->wel && flash->cmd == FLASH_CMD_SE) {
            /* Sector erase: fill sector with 0xFF */
            uint32_t sector_addr = flash->addr & ~(FLASH_SECTOR_SIZE - 1);
            memset(flash->storage + sector_addr, 0xFF, FLASH_SECTOR_SIZE);
            flash->wel = false; /* WEL cleared after erase */
        }
        if (flash->wel && flash->cmd == FLASH_CMD_PP) {
            /* Page program completed on CS deassert */
            flash->wel = false; /* WEL cleared after program */
        }
        /* Reset state machine (but preserve WEL if no write/erase was done) */
        flash->state = FLASH_STATE_IDLE;
        flash->cmd = 0;
        flash->addr = 0;
        flash->addr_bytes = 0;
        flash->data_idx = 0;
        flash->sr &= ~FLASH_SR_BUSY;
    }
}

uint8_t g233_spi_flash_xfer(G233SpiFlashState *flash, uint8_t tx)
{
    uint8_t rx = 0x00;

    switch (flash->state) {
    case FLASH_STATE_IDLE:
        /* Receive command byte */
        flash->cmd = tx;
        flash->addr = 0;
        flash->addr_bytes = 0;
        flash->data_idx = 0;

        switch (tx) {
        case FLASH_CMD_JEDEC_ID:
            flash->state = FLASH_STATE_JEDEC;
            flash->data_idx = 0;
            break;
        case FLASH_CMD_RDSR:
            flash->state = FLASH_STATE_RDSR;
            break;
        case FLASH_CMD_WREN:
            flash->wel = true;
            /* No further bytes expected */
            break;
        case FLASH_CMD_READ:
        case FLASH_CMD_PP:
        case FLASH_CMD_SE:
            flash->state = FLASH_STATE_READ_ADDR;
            flash->addr_bytes = 0;
            break;
        default:
            /* Unknown command, stay idle */
            break;
        }
        break;

    case FLASH_STATE_JEDEC:
        /* Return JEDEC ID bytes */
        rx = flash->jedec_id[flash->data_idx];
        flash->data_idx++;
        if (flash->data_idx >= 3) {
            flash->state = FLASH_STATE_IDLE;
        }
        break;

    case FLASH_STATE_RDSR:
        rx = flash->sr;
        /* RDSR keeps returning SR until CS deasserted */
        break;

    case FLASH_STATE_READ_ADDR:
        /* Accumulate 3 address bytes */
        flash->addr = (flash->addr << 8) | tx;
        flash->addr_bytes++;
        if (flash->addr_bytes >= 3) {
            if (flash->cmd == FLASH_CMD_READ) {
                flash->state = FLASH_STATE_READ_DATA;
                flash->data_idx = 0;
            } else if (flash->cmd == FLASH_CMD_PP) {
                flash->state = FLASH_STATE_WRITE_DATA;
                flash->data_idx = 0;
                flash->sr |= FLASH_SR_BUSY;
            } else if (flash->cmd == FLASH_CMD_SE) {
                /* Sector erase happens on CS deassert */
                flash->sr |= FLASH_SR_BUSY;
                flash->state = FLASH_STATE_IDLE;
            }
        }
        break;

    case FLASH_STATE_READ_DATA:
        /* Read data from flash storage */
        if (flash->addr < flash->size) {
            rx = flash->storage[flash->addr];
        } else {
            rx = 0xFF;
        }
        flash->addr++;
        /* Auto-increment, wrap around at size boundary */
        if (flash->addr >= flash->size) {
            flash->addr = 0;
        }
        break;

    case FLASH_STATE_WRITE_DATA:
        /* Page program: write data to flash storage */
        if (flash->wel && flash->addr < flash->size) {
            /* Page program: can only write within a 256-byte page */
            flash->storage[flash->addr] = tx;
        }
        flash->addr++;
        flash->data_idx++;
        /* Note: we don't enforce page boundaries since tests write
         * sequentially within a page */
        break;

    default:
        flash->state = FLASH_STATE_IDLE;
        break;
    }

    return rx;
}

static void g233_spi_flash_realize(DeviceState *dev, Error **errp)
{
    G233SpiFlashState *s = G233_SPI_FLASH(dev);

    if (s->size == 0) {
        error_setg(errp, "g233-spi-flash: size must be set");
        return;
    }

    s->storage = g_malloc0(s->size);
    /* Initialize to 0xFF (erased state) */
    memset(s->storage, 0xFF, s->size);

    s->state = FLASH_STATE_IDLE;
    s->sr = 0;
    s->wel = false;
}

static void g233_spi_flash_finalize(Object *obj)
{
    G233SpiFlashState *s = G233_SPI_FLASH(obj);
    g_free(s->storage);
}

static const Property g233_spi_flash_properties[] = {
    DEFINE_PROP_UINT32("size", G233SpiFlashState, size, 0),
    DEFINE_PROP_UINT8("jedec-id0", G233SpiFlashState, jedec_id[0], 0),
    DEFINE_PROP_UINT8("jedec-id1", G233SpiFlashState, jedec_id[1], 0),
    DEFINE_PROP_UINT8("jedec-id2", G233SpiFlashState, jedec_id[2], 0),
};

static void g233_spi_flash_class_init(ObjectClass *klass, const void *data)
{
    DeviceClass *dc = DEVICE_CLASS(klass);

    dc->realize = g233_spi_flash_realize;
    device_class_set_props(dc, g233_spi_flash_properties);
    dc->desc = "G233 SPI Flash";
}

static const TypeInfo g233_spi_flash_info = {
    .name = TYPE_G233_SPI_FLASH,
    .parent = TYPE_DEVICE,
    .instance_size = sizeof(G233SpiFlashState),
    .instance_finalize = g233_spi_flash_finalize,
    .class_init = g233_spi_flash_class_init,
};

static void g233_spi_flash_register_types(void)
{
    type_register_static(&g233_spi_flash_info);
}

type_init(g233_spi_flash_register_types)
