/*
 * xbased native core — the QC (JVS and backup) and SC (serial) USB devices.
 */
#ifndef NATIVE_DEV_CHIHIRO_USB_H
#define NATIVE_DEV_CHIHIRO_USB_H

#include "native/dev/usb.h"
#include "native/dev/chihiro_board.h"

#include "crp1231.h"
#include "jvs.h"
#include "v257.h"

enum {
    CHIHIRO_EEPROM_SIZE = 8192,
    CHIHIRO_SETTINGS_SIZE = 256,
    CHIHIRO_SRAM_SIZE = 65536,
    CHIHIRO_USB_PACKET = 64,
    CHIHIRO_USB_ENDPOINTS = 6,
};

typedef struct ChihiroUsbTransfer {
    uint8_t *bytes;
    unsigned left;
} ChihiroUsbTransfer;

typedef struct ChihiroUart {
    uint8_t config[11];             /* control requests 0x25–0x2f */
    uint64_t tx_until_ns;
    uint64_t rx_next_ns;
} ChihiroUart;

typedef struct ChihiroUsbBoard {
    NcUsbDev usb;
    NcJvs jvs;
    bool qc;
    bool cpu_held;
    bool backup_dirty;
    uint8_t eeprom[CHIHIRO_EEPROM_SIZE];
    uint8_t settings[CHIHIRO_SETTINGS_SIZE];
    uint8_t sram[CHIHIRO_SRAM_SIZE];
    uint8_t rtc[16];
    int64_t rtc_offset;
    ChihiroUsbTransfer in[CHIHIRO_USB_ENDPOINTS], out[CHIHIRO_USB_ENDPOINTS];
    uint8_t jvs_tx[512];
    unsigned jvs_tx_len;
    uint8_t jvs_reply[6 + 2 * 260];
    unsigned jvs_reply_len;
    ChihiroUart uart[2];
    uint8_t rx[2][CHIHIRO_USB_PACKET];
    uint8_t port_b;
} ChihiroUsbBoard;

void chihiro_usb_init(ChihiroUsbBoard *board, bool qc);
void chihiro_usb_apply_coins(ChihiroUsbBoard *board, const NcChihiroCoinSettings *coins);

/* What hangs off the SC's UARTs (chihiro_board.c owns them): the card
 * reader on the first, the steering board on the second. */
extern NcCardReader nc_chihiro_card_reader;
extern NcV257 nc_chihiro_v257;

#endif
