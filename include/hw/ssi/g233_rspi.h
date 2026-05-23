/*
 * G233 Rust SPI Controller header
 *
 * SPDX-License-Identifier: GPL-2.0-or-later
 */

#ifndef HW_SSI_G233_RSPI_H
#define HW_SSI_G233_RSPI_H

#include "hw/core/sysbus.h"

#define TYPE_G233_RSPI "g233-rspi"
OBJECT_DECLARE_SIMPLE_TYPE(G233RspiState, G233_RSPI)

#define G233_RSPI_SIZE  0x1000
#define G233_RSPI_NUM_CS 2

/* Register offsets */
#define G233_RSPI_CR1   0x00
#define G233_RSPI_SR    0x04
#define G233_RSPI_DR    0x08
#define G233_RSPI_CS    0x0C

/* CR1 bits */
#define G233_RSPI_CR1_SPE     (1u << 0)
#define G233_RSPI_CR1_MSTR    (1u << 2)

/* SR bits */
#define G233_RSPI_SR_RXNE     (1u << 0)
#define G233_RSPI_SR_TXE      (1u << 1)
#define G233_RSPI_SR_OVERRUN  (1u << 4)

/* AT25 SPI flash state */
typedef enum {
    AT25_STATE_IDLE,
    AT25_STATE_RDSR,
    AT25_STATE_READ_ADDR,
    AT25_STATE_READ_DATA,
    AT25_STATE_WRITE_ADDR,
    AT25_STATE_WRITE_DATA,
} At25State;

#define AT25_FLASH_SIZE 256

typedef struct {
    At25State state;
    uint8_t cmd;
    uint8_t sr;         /* Status register: bit0=WIP, bit1=WEL */
    uint8_t storage[AT25_FLASH_SIZE];
    uint32_t addr;
    int addr_bytes;
    bool wel;           /* Write Enable Latch */
} At25Flash;

struct G233RspiState {
    /*< private >*/
    SysBusDevice parent_obj;

    /*< public >*/
    MemoryRegion mmio;
    qemu_irq irq;

    /* Registers */
    uint32_t cr1;
    uint32_t sr;
    uint32_t rx_data;
    uint32_t cs_reg;

    /* Embedded AT25 flash on CS0 */
    At25Flash flash;
};

#endif
