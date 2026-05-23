/*
 * QEMU model of the G233 SPI Controller
 *
 * SPDX-License-Identifier: GPL-2.0-or-later
 */

#include "qemu/osdep.h"
#include "hw/core/irq.h"
#include "hw/core/qdev-properties.h"
#include "hw/ssi/g233_spi.h"
#include "migration/vmstate.h"
#include "qemu/log.h"
#include "qemu/module.h"

void g233_spi_set_flash(G233SpiState *s, int cs, G233SpiFlashState *flash)
{
    if (cs >= 0 && cs < G233_SPI_NUM_CS) {
        s->flash[cs] = flash;
    }
}

static void g233_spi_update_irq(G233SpiState *s)
{
    int level = 0;

    if (!(s->cr1 & G233_SPI_CR1_SPE)) {
        qemu_set_irq(s->irq, 0);
        return;
    }

    /* TXE interrupt */
    if ((s->cr1 & G233_SPI_CR1_TXEIE) && (s->sr & G233_SPI_SR_TXE)) {
        level = 1;
    }

    /* RXNE interrupt */
    if ((s->cr1 & G233_SPI_CR1_RXNEIE) && (s->sr & G233_SPI_SR_RXNE)) {
        level = 1;
    }

    /* Error interrupt (overrun) */
    if ((s->cr1 & G233_SPI_CR1_ERRIE) && (s->sr & G233_SPI_SR_OVERRUN)) {
        level = 1;
    }

    qemu_set_irq(s->irq, level);
}

/* Get the currently selected flash device */
static G233SpiFlashState *g233_spi_get_flash(G233SpiState *s)
{
    int cs = s->cr2 & 0x3;
    if (cs < G233_SPI_NUM_CS && s->flash[cs]) {
        return s->flash[cs];
    }
    return NULL;
}

static uint64_t g233_spi_read(void *opaque, hwaddr offset, unsigned size)
{
    G233SpiState *s = G233_SPI(opaque);

    switch (offset) {
    case G233_SPI_CR1:
        return s->cr1;
    case G233_SPI_CR2:
        return s->cr2;
    case G233_SPI_SR:
        return s->sr;
    case G233_SPI_DR: {
        /* Reading DR clears RXNE */
        uint32_t val = s->sr & 0xFF; /* Return last received byte stored in SR low bits */
        /*
         * Actually, we need a separate rx_data field. Let me use a simple
         * approach: the DR read returns the last byte received from flash.
         * We store it in the upper bits of SR for simplicity... no, let's
         * add a proper rx_data field.
         */
        /* For now, we store rx_data in the state */
        val = s->rx_data;
        s->sr &= ~G233_SPI_SR_RXNE;
        g233_spi_update_irq(s);
        return val;
    }
    default:
        qemu_log_mask(LOG_GUEST_ERROR,
                      "%s: bad read offset 0x%" HWADDR_PRIx "\n",
                      __func__, offset);
        return 0;
    }
}

static void g233_spi_write(void *opaque, hwaddr offset,
                            uint64_t value, unsigned size)
{
    G233SpiState *s = G233_SPI(opaque);

    switch (offset) {
    case G233_SPI_CR1: {
        uint32_t old_cr1 = s->cr1;
        s->cr1 = value;

        /* When SPE transitions to 1, set TXE */
        if ((value & G233_SPI_CR1_SPE) && !(old_cr1 & G233_SPI_CR1_SPE)) {
            s->sr |= G233_SPI_SR_TXE;
        }
        /* When SPE transitions to 0, clear status */
        if (!(value & G233_SPI_CR1_SPE) && (old_cr1 & G233_SPI_CR1_SPE)) {
            s->sr = G233_SPI_SR_TXE;
        }

        g233_spi_update_irq(s);
        break;
    }
    case G233_SPI_CR2: {
        int old_cs = s->cr2 & 0x3;
        int new_cs = value & 0x3;
        s->cr2 = value;

        /* CS change: deselect old, select new */
        if (old_cs != new_cs) {
            if (old_cs < G233_SPI_NUM_CS && s->flash[old_cs]) {
                g233_spi_flash_cs_change(s->flash[old_cs], false);
            }
        }
        break;
    }
    case G233_SPI_SR:
        /* Write-1-to-clear OVERRUN */
        if (value & G233_SPI_SR_OVERRUN) {
            s->sr &= ~G233_SPI_SR_OVERRUN;
            g233_spi_update_irq(s);
        }
        break;
    case G233_SPI_DR: {
        if (!(s->cr1 & G233_SPI_CR1_SPE)) {
            break;
        }

        /* Check for overrun: if RXNE is already set */
        if (s->sr & G233_SPI_SR_RXNE) {
            s->sr |= G233_SPI_SR_OVERRUN;
        }

        /* Perform SPI transfer */
        G233SpiFlashState *flash = g233_spi_get_flash(s);
        if (flash) {
            s->rx_data = g233_spi_flash_xfer(flash, (uint8_t)value);
        } else {
            s->rx_data = 0xFF;
        }

        /* Set RXNE, TXE stays set (immediately ready for next byte) */
        s->sr |= G233_SPI_SR_RXNE;
        s->sr |= G233_SPI_SR_TXE;

        g233_spi_update_irq(s);
        break;
    }
    default:
        qemu_log_mask(LOG_GUEST_ERROR,
                      "%s: bad write offset 0x%" HWADDR_PRIx "\n",
                      __func__, offset);
    }
}

static const MemoryRegionOps g233_spi_ops = {
    .read = g233_spi_read,
    .write = g233_spi_write,
    .endianness = DEVICE_LITTLE_ENDIAN,
    .impl.min_access_size = 4,
    .impl.max_access_size = 4,
};

static void g233_spi_reset(DeviceState *dev)
{
    G233SpiState *s = G233_SPI(dev);

    s->cr1 = 0;
    s->cr2 = 0;
    s->sr = G233_SPI_SR_TXE; /* TXE set at reset */
    s->rx_data = 0;

    /* Deselect all flash devices */
    for (int i = 0; i < G233_SPI_NUM_CS; i++) {
        if (s->flash[i]) {
            g233_spi_flash_cs_change(s->flash[i], false);
        }
    }
}

static void g233_spi_realize(DeviceState *dev, Error **errp)
{
    G233SpiState *s = G233_SPI(dev);

    memory_region_init_io(&s->mmio, OBJECT(dev), &g233_spi_ops, s,
                          TYPE_G233_SPI, G233_SPI_SIZE);
    sysbus_init_mmio(SYS_BUS_DEVICE(dev), &s->mmio);
    sysbus_init_irq(SYS_BUS_DEVICE(dev), &s->irq);
}

static const VMStateDescription vmstate_g233_spi = {
    .name = TYPE_G233_SPI,
    .version_id = 1,
    .minimum_version_id = 1,
    .fields = (const VMStateField[]) {
        VMSTATE_UINT32(cr1, G233SpiState),
        VMSTATE_UINT32(cr2, G233SpiState),
        VMSTATE_UINT32(sr, G233SpiState),
        VMSTATE_UINT32(rx_data, G233SpiState),
        VMSTATE_END_OF_LIST()
    }
};

static void g233_spi_class_init(ObjectClass *klass, const void *data)
{
    DeviceClass *dc = DEVICE_CLASS(klass);

    device_class_set_legacy_reset(dc, g233_spi_reset);
    dc->vmsd = &vmstate_g233_spi;
    dc->realize = g233_spi_realize;
    dc->desc = "G233 SPI";
}

static const TypeInfo g233_spi_info = {
    .name = TYPE_G233_SPI,
    .parent = TYPE_SYS_BUS_DEVICE,
    .instance_size = sizeof(G233SpiState),
    .class_init = g233_spi_class_init,
};

static void g233_spi_register_types(void)
{
    type_register_static(&g233_spi_info);
}

type_init(g233_spi_register_types)
