/*
 * G233 SPI Flash header
 *
 * SPDX-License-Identifier: GPL-2.0-or-later
 */

#ifndef HW_SSI_G233_SPI_FLASH_H
#define HW_SSI_G233_SPI_FLASH_H

#include "hw/core/sysbus.h"

#define TYPE_G233_SPI_FLASH "g233-spi-flash"
OBJECT_DECLARE_SIMPLE_TYPE(G233SpiFlashState, G233_SPI_FLASH)

/* Flash command opcodes */
#define FLASH_CMD_WREN      0x06
#define FLASH_CMD_RDSR      0x05
#define FLASH_CMD_READ      0x03
#define FLASH_CMD_PP        0x02
#define FLASH_CMD_SE        0x20
#define FLASH_CMD_JEDEC_ID  0x9F

/* Flash status register bits */
#define FLASH_SR_BUSY       0x01

#define FLASH_PAGE_SIZE     256
#define FLASH_SECTOR_SIZE   4096

/* Flash state machine states */
typedef enum {
    FLASH_STATE_IDLE,
    FLASH_STATE_CMD,
    FLASH_STATE_READ_ADDR,
    FLASH_STATE_READ_DATA,
    FLASH_STATE_WRITE_ADDR,
    FLASH_STATE_WRITE_DATA,
    FLASH_STATE_RDSR,
    FLASH_STATE_JEDEC,
} FlashState;

struct G233SpiFlashState {
    /*< private >*/
    DeviceState parent_obj;

    /*< public >*/
    /* JEDEC ID bytes */
    uint8_t jedec_id[3];

    /* Flash storage */
    uint32_t size;
    uint8_t *storage;

    /* State machine */
    FlashState state;
    uint8_t cmd;
    int addr_bytes;
    uint32_t addr;
    int data_idx;

    /* Status */
    uint8_t sr;
    bool wel; /* Write Enable Latch */
};

uint8_t g233_spi_flash_xfer(G233SpiFlashState *flash, uint8_t tx);
void g233_spi_flash_cs_change(G233SpiFlashState *flash, bool selected);

#endif
