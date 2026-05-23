/*
 * G233 SPI Controller header
 *
 * SPDX-License-Identifier: GPL-2.0-or-later
 */

#ifndef HW_SSI_G233_SPI_H
#define HW_SSI_G233_SPI_H

#include "hw/core/sysbus.h"
#include "hw/ssi/g233_spi_flash.h"

#define TYPE_G233_SPI "g233-spi"
OBJECT_DECLARE_SIMPLE_TYPE(G233SpiState, G233_SPI)

#define G233_SPI_SIZE  0x1000
#define G233_SPI_NUM_CS 2

/* Register offsets */
#define G233_SPI_CR1   0x00
#define G233_SPI_CR2   0x04
#define G233_SPI_SR    0x08
#define G233_SPI_DR    0x0C

/* CR1 bits */
#define G233_SPI_CR1_SPE     (1u << 0)
#define G233_SPI_CR1_MSTR    (1u << 2)
#define G233_SPI_CR1_ERRIE   (1u << 5)
#define G233_SPI_CR1_RXNEIE  (1u << 6)
#define G233_SPI_CR1_TXEIE   (1u << 7)

/* SR bits */
#define G233_SPI_SR_RXNE     (1u << 0)
#define G233_SPI_SR_TXE      (1u << 1)
#define G233_SPI_SR_OVERRUN  (1u << 4)

struct G233SpiState {
    /*< private >*/
    SysBusDevice parent_obj;

    /*< public >*/
    MemoryRegion mmio;
    qemu_irq irq;

    uint32_t cr1;
    uint32_t cr2;
    uint32_t sr;
    uint32_t rx_data; /* Last received byte */

    /* Flash backends */
    G233SpiFlashState *flash[G233_SPI_NUM_CS];
};

/* Set flash backend for a CS line */
void g233_spi_set_flash(G233SpiState *s, int cs, G233SpiFlashState *flash);

#endif
