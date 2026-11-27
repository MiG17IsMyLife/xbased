/*
 * Chihiro baseboard USB devices, QC and SC (internal to native/dev/chihiro).
 *
 * Both are Cypress EZ-USB chips: an 8051 microcontroller behind a USB
 * port. The game talks to them with vendor control requests (one per
 * operation) and moves the data on bulk endpoints.
 *
 *   QC  JVS bus, firmware/settings EEPROMs, backup SRAM, real-time clock
 *       baseboard_qc.c
 *   SC  two serial ports (UARTs): the card reader and the drive board
 *       baseboard_sc.c
 *
 * baseboard_usb.c has what both share: USB descriptors, request dispatch
 * and endpoint transfers. coin_settings.c fills the settings EEPROM.
 */
#ifndef NATIVE_DEV_CHIHIRO_BASEBOARD_H
#define NATIVE_DEV_CHIHIRO_BASEBOARD_H

#include "native/dev/usb.h"
#include "native/dev/chihiro/board.h"

#include "crp1231.h"
#include "drive.h"
#include "jvs.h"

enum {
    CHIHIRO_EEPROM_SIZE = 8192,     /* the 8051's firmware EEPROM */
    CHIHIRO_SETTINGS_SIZE = 256,    /* settings EEPROM (coin settings, ...) */
    CHIHIRO_SRAM_SIZE = 65536,      /* battery-backed SRAM, the game's backup */
    CHIHIRO_USB_PACKET = 64,        /* bulk packet size */
    CHIHIRO_USB_ENDPOINTS = 6,
};

/* Bulk endpoint numbers. Each request queues bytes on one of them. */
enum {
    EP_FIRMWARE_EEPROM = 1,
    EP_SETTINGS_OR_CARD = 2,    /* QC: settings EEPROM;  SC: card reader UART */
    EP_BACKUP_OR_DRIVE = 3,     /* QC: backup SRAM;      SC: drive board UART */
    EP_JVS = 4,                 /* QC only */
    EP_RTC = 5,                 /* QC only */
};

/* A JVS frame through the QC carries a 3-byte header going out and a
 * 6-byte header coming back, in front of the JVS bytes. */
enum { JVS_HEADER_BYTES = 3, JVS_REPLY_HEADER_BYTES = 6 };

/* Requests 0x25-0x2f configure an SC UART; 0x2e is its control register. */
enum { SC_UART_FIRST = 0x25, SC_UART_CONTROL = 0x2e, SC_UART_LAST = 0x2f };

/* Bytes still to move on one endpoint. */
typedef struct ChihiroUsbTransfer {
    uint8_t *bytes;
    unsigned left;
} ChihiroUsbTransfer;

/* One SC serial port. The times pace bytes at the line's real speed. */
typedef struct ChihiroUart {
    uint8_t config[11];             /* values of requests 0x25-0x2f */
    uint64_t tx_until_ns;           /* when the bytes sent so far finish */
    uint64_t rx_next_ns;            /* when the next byte may come back */
} ChihiroUart;

/* One QC or SC device. Fields only one of them uses are marked. */
typedef struct ChihiroUsbBoard {
    NcUsbDev usb;
    bool qc;                        /* true QC, false SC */
    bool cpu_held;                  /* the game holds the 8051 in reset */
    uint8_t eeprom[CHIHIRO_EEPROM_SIZE];
    ChihiroUsbTransfer in[CHIHIRO_USB_ENDPOINTS], out[CHIHIRO_USB_ENDPOINTS];

    /* QC */
    NcJvs jvs;
    bool backup_dirty;              /* settings or SRAM changed since the last save */
    void (*backup_written)(void);   /* the game finished writing to the SRAM */
    uint8_t settings[CHIHIRO_SETTINGS_SIZE];
    uint8_t sram[CHIHIRO_SRAM_SIZE];
    uint8_t rtc[16];
    int64_t rtc_offset;             /* seconds the game's clock is ahead of the host */
    uint8_t jvs_tx[512];
    unsigned jvs_tx_len;
    uint8_t jvs_reply[6 + 2 * 260];
    unsigned jvs_reply_len;

    /* SC */
    ChihiroUart uart[2];            /* 0 card reader, 1 drive board */
    uint8_t rx[2][CHIHIRO_USB_PACKET];
    uint8_t port_b;
} ChihiroUsbBoard;

/* Sets up a QC (qc true) or SC device with empty memory. */
void chihiro_usb_init(ChihiroUsbBoard *board, bool qc);

/* What the SC's serial ports are connected to (both live in board.c and
 * cards.c): the card reader, and the game's drive board (NULL: none). */
extern NcCardReader nc_chihiro_card_reader;
extern const NcChihiroDrive *nc_chihiro_drive;

/* ---- shared by the baseboard_*.c files ---- */

/* Points transfer x at count bytes of memory, starting at offset.
 * False if that runs past the end of memory (size bytes). */
static inline bool chihiro_usb_queue(ChihiroUsbTransfer *x, uint8_t *memory, unsigned size,
                                     unsigned offset, unsigned count)
{
    if (offset > size || count > size - offset) {
        return false;
    }
    *x = (ChihiroUsbTransfer){memory + offset, count};
    return true;
}

/* QC (baseboard_qc.c) */
void chihiro_qc_status(const ChihiroUsbBoard *b, uint8_t reply[8]);
bool chihiro_qc_request(ChihiroUsbBoard *b, uint8_t request, unsigned offset, unsigned count, uint8_t reply[8]);
void chihiro_qc_out_done(ChihiroUsbBoard *b, int ep);

/* SC (baseboard_sc.c) */
void chihiro_sc_status(const ChihiroUsbBoard *b, uint8_t request, uint8_t reply[8]);
bool chihiro_sc_request(ChihiroUsbBoard *b, uint8_t request, unsigned value, unsigned count, uint8_t reply[8]);
void chihiro_sc_send(ChihiroUsbBoard *b, unsigned channel, const uint8_t *bytes, unsigned n);

/* Settings EEPROM (coin_settings.c) */
void chihiro_coin_settings_init(ChihiroUsbBoard *b);
void chihiro_usb_apply_coins(ChihiroUsbBoard *board, const NcChihiroCoinSettings *coins);

#endif
