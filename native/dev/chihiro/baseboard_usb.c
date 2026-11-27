/*
 * Chihiro baseboard USB: what the QC and SC devices share (baseboard.h).
 *
 * The game sends a vendor control request for each operation ("read the
 * SRAM", "send on UART 1", ...). The request queues bytes on a bulk
 * endpoint, and the game then reads or writes them there. board_control
 * handles the requests, board_data the bulk transfers.
 */
#include "baseboard.h"
#include "trace.h"

#include <string.h>

enum {
    REQ_ANCHOR_LOAD = 0xa0,     /* EZ-USB: write to the 8051's memory */
    CPU_CONTROL = 0x7f92,       /* EZ-USB: the 8051's reset register */
};

/* ---------------------------------------------------------- descriptors */

#define BULK(ep) 7, 5, (ep), 2, CHIHIRO_USB_PACKET, 0, 0
#define CONFIG(count) \
    9, 2, (18 + 7 * (count)), 0, 1, 1, 0, 0x80, 0x96, \
    9, 4, 0, 0, (count), 0xff, 0, 0, 0

/* Sega's vendor ID 0x0ca3; product 0x0002 (QC) and 0x0003 (SC). */
static const uint8_t k_qc_device[] = {
    18, 1, 0, 1, 0x60, 0, 0, 64, 0xa3, 0x0c, 2, 0, 8, 1, 1, 2, 0, 1,
};
static const uint8_t k_sc_device[] = {
    18, 1, 0, 1, 0x60, 1, 0, 64, 0xa3, 0x0c, 3, 0, 0x10, 1, 1, 2, 0, 1,
};
/* QC: bulk endpoints 1-5 each way. SC: 1-3 each way. */
static const uint8_t k_qc_config[] = {
    CONFIG(10), BULK(1), BULK(2), BULK(3), BULK(4), BULK(5),
    BULK(0x81), BULK(0x82), BULK(0x83), BULK(0x84), BULK(0x85),
};
static const uint8_t k_sc_config[] = {
    CONFIG(6), BULK(1), BULK(2), BULK(3), BULK(0x81), BULK(0x82), BULK(0x83),
};
static const char *const k_strings[] = {"SEGA", "BASEBD"};

_Static_assert(sizeof(k_qc_config) == 88 && sizeof(k_sc_config) == 60, "USB configuration lengths");

#undef CONFIG
#undef BULK

/* ---------------------------------------------------------- requests */

static void reset_transfers(ChihiroUsbBoard *b)
{
    memset(b->in, 0, sizeof(b->in));
    memset(b->out, 0, sizeof(b->out));
    memset(b->uart, 0, sizeof(b->uart));
    b->jvs_tx_len = 0;
    b->jvs_reply_len = 0;
}

static void board_reset(NcUsbDev *dev)
{
    reset_transfers(dev->opaque);
}

/* A control request. Every vendor request answers 8 status bytes. */
static int board_control(NcUsbDev *dev, const uint8_t setup[8], uint8_t *data, int len)
{
    ChihiroUsbBoard *b = dev->opaque;
    const uint16_t value = nc_rd16(setup + 2);
    const uint16_t count = nc_rd16(setup + 4);
    const uint8_t request = setup[1];
    static unsigned budget = 180;
    if (chihiro_trace(&budget)) {
        nc_log("xbased-chihiro", "%s: request %02x:%02x value %04x index %04x length %d",
               dev->name, setup[0], request, value, count, len);
    }

    /* The game holds the 8051 in reset while it (re)loads its firmware. */
    if (setup[0] == 0x40 && request == REQ_ANCHOR_LOAD && value == CPU_CONTROL && len > 0) {
        b->cpu_held = (data[0] & 1) != 0;
        if (b->cpu_held) {
            reset_transfers(b);
        }
        return 0;
    }
    /* Everything else must be a vendor IN request to a running 8051. */
    if (setup[0] != 0xc0 || b->cpu_held) {
        static unsigned told;
        if (told++ < 40) {
            nc_log("xbased-chihiro", "%s: request %02x:%02x value %04x refused (%s)", dev->name, setup[0], request,
                   value, b->cpu_held ? "8051 held" : "not a vendor IN request");
        }
        return NC_USB_STALL;
    }

    uint8_t reply[8] = {0};
    if (b->qc) {
        chihiro_qc_status(b, reply);
    } else {
        chihiro_sc_status(b, request, reply);
    }
    bool handled;
    if (request == 0x16 || (b->qc && request == 0x1d)) {
        /* Firmware EEPROM: 0x16 read (both), 0x1d write (QC only). */
        ChihiroUsbTransfer *x = request == 0x16 ? b->in : b->out;
        handled = chihiro_usb_queue(&x[EP_FIRMWARE_EEPROM], b->eeprom, sizeof(b->eeprom), value, count);
    } else if (b->qc) {
        handled = chihiro_qc_request(b, request, value, count, reply);
    } else {
        handled = chihiro_sc_request(b, request, value, count, reply);
    }
    if (!handled) {
        static unsigned told;
        if (told++ < 40) {
            nc_log("xbased-chihiro", "%s: unsupported request %02x value %04x count %04x", dev->name, request, value, count);
        }
        return NC_USB_STALL;
    }
    static unsigned sc_budget = 400;
    if (!b->qc && chihiro_trace(&sc_budget)) {
        nc_log("xbased-chihiro", "sc: %02x v%04x i%04x -> %02x %02x %02x %02x %02x %02x %02x %02x", request, value, count,
               reply[0], reply[1], reply[2], reply[3], reply[4], reply[5], reply[6], reply[7]);
    }
    const int n = MIN(len, (int)sizeof(reply));
    memcpy(data, reply, (size_t)n);
    return n;
}

/* A bulk transfer on endpoint ep: moves up to one packet of the bytes the
 * last request queued there. NAK when nothing is queued. */
static int board_data(NcUsbDev *dev, bool in, int ep, uint8_t *bytes, int len)
{
    ChihiroUsbBoard *b = dev->opaque;
    if (ep < 1 || ep > (b->qc ? EP_RTC : EP_BACKUP_OR_DRIVE) || b->cpu_held) {
        return NC_USB_STALL;
    }
    ChihiroUsbTransfer *x = in ? &b->in[ep] : &b->out[ep];
    if (!x->left) {
        return NC_USB_NAK;
    }
    const unsigned n = MIN(MIN((unsigned)len, (unsigned)CHIHIRO_USB_PACKET), x->left);
    if (in) {
        memcpy(bytes, x->bytes, n);
    } else if (!b->qc && ep >= EP_SETTINGS_OR_CARD) {
        /* SC: the bytes go out on a serial port, not into memory. */
        chihiro_sc_send(b, (unsigned)ep - EP_SETTINGS_OR_CARD, bytes, n);
    } else {
        if (b->qc && (ep == EP_SETTINGS_OR_CARD || ep == EP_BACKUP_OR_DRIVE) && memcmp(x->bytes, bytes, n)) {
            b->backup_dirty = true;
        }
        memcpy(x->bytes, bytes, n);
    }
    if (x->bytes) {
        x->bytes += n;
    }
    x->left -= n;
    if (!in && b->qc && !x->left) {
        chihiro_qc_out_done(b, ep);
    }
    return (int)n;
}

/* ---------------------------------------------------------- setup */

void chihiro_usb_init(ChihiroUsbBoard *b, bool qc)
{
    memset(b, 0, sizeof(*b));
    b->qc = qc;
    b->usb = (NcUsbDev){
        .name = qc ? "chihiro-qc" : "chihiro-sc",
        .dev_desc = qc ? k_qc_device : k_sc_device,
        .dev_desc_len = sizeof(k_qc_device),
        .cfg_desc = qc ? k_qc_config : k_sc_config,
        .cfg_desc_len = qc ? sizeof(k_qc_config) : sizeof(k_sc_config),
        .strings = k_strings,
        .nstrings = sizeof(k_strings) / sizeof(k_strings[0]),
        .control = board_control,
        .data = board_data,
        .reset = board_reset,
        .opaque = b,
        .pending_addr = -1,
    };
    /* An EZ-USB firmware EEPROM: header byte 0xb2, then the USB vendor ID,
     * product ID and release; the rest erased (0xff). */
    memset(b->eeprom, 0xff, sizeof(b->eeprom));
    const uint8_t *desc = b->usb.dev_desc;
    b->eeprom[0] = 0xb2;
    memcpy(b->eeprom + 1, desc + 8, 6);
    if (qc) {
        /* The cabinet's region (here Japan) and its bitwise complement,
         * which the game checks. board.c may change it. */
        b->eeprom[0x1f00] = 1;
        b->eeprom[0x1f01] = (uint8_t)~b->eeprom[0x1f00];
        nc_jvs_init(&b->jvs);
        chihiro_coin_settings_init(b);
    }
}
