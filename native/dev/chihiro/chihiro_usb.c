/*
 * xbased native core — the Chihiro baseboard's USB protocol.
 * The QC bridges USB to its EEPROMs, SRAM, RTC and JVS bus. The SC bridges
 * USB to the card reader and drive controller on its two UARTs.
 */
#include "chihiro_usb.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

enum {
    EP_FIRMWARE_EEPROM = 1,
    EP_SETTINGS_OR_CARD = 2,
    EP_BACKUP_OR_DRIVE = 3,
    EP_JVS = 4,
    EP_RTC = 5,
    REQ_ANCHOR_LOAD = 0xa0,
    CPU_CONTROL = 0x7f92,
    SC_UART_FIRST = 0x25,
    SC_UART_CONTROL = 0x2e,
    SC_UART_LAST = 0x2f,
    JVS_HEADER_BYTES = 3,
    JVS_REPLY_HEADER_BYTES = 6,
};

uint64_t nc_ke_now_ns(void);

/* NC_TRACE_CHIHIRO: up to *budget lines of USB traffic for this call site */
static bool tracing(unsigned *budget)
{
    static int on = -1;
    if (on < 0) {
        on = getenv("NC_TRACE_CHIHIRO") != NULL;
    }
    return on && *budget && (*budget)--;
}

static const char *hex(const uint8_t *b, unsigned n, char out[3 * 24 + 1])
{
    out[0] = 0;
    for (unsigned i = 0; i < n && i < 24; i++) {
        snprintf(out + 3 * i, 4, " %02x", b[i]);
    }
    return out;
}

#define BULK(ep) 7, 5, (ep), 2, CHIHIRO_USB_PACKET, 0, 0
#define CONFIG(count) \
    9, 2, (18 + 7 * (count)), 0, 1, 1, 0, 0x80, 0x96, \
    9, 4, 0, 0, (count), 0xff, 0, 0, 0

static const uint8_t k_qc_device[] = {
    18, 1, 0, 1, 0x60, 0, 0, 64, 0xa3, 0x0c, 2, 0, 8, 1, 1, 2, 0, 1,
};
static const uint8_t k_sc_device[] = {
    18, 1, 0, 1, 0x60, 1, 0, 64, 0xa3, 0x0c, 3, 0, 0x10, 1, 1, 2, 0, 1,
};
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

static bool queue_bytes(ChihiroUsbTransfer *x, uint8_t *memory, unsigned size, unsigned offset, unsigned count)
{
    if (offset > size || count > size - offset) {
        return false;
    }
    *x = (ChihiroUsbTransfer){memory + offset, count};
    return true;
}

static unsigned bcd(unsigned val)
{
    return (val / 10) * 16 + val % 10;
}

static int unbcd(uint8_t val)
{
    return (val >> 4) * 10 + (val & 15);
}

static void rtc_read(ChihiroUsbBoard *b)
{
    const time_t now = time(NULL) + b->rtc_offset;
    const struct tm *t = localtime(&now);
    if (t) {
        b->rtc[0] = (uint8_t)bcd(t->tm_sec);
        b->rtc[1] = (uint8_t)bcd(t->tm_min);
        b->rtc[2] = (uint8_t)bcd(t->tm_hour);
        b->rtc[3] = (uint8_t)(t->tm_wday + 1);
        b->rtc[4] = (uint8_t)bcd(t->tm_mday);
        b->rtc[5] = (uint8_t)bcd(t->tm_mon + 1);
        b->rtc[6] = (uint8_t)bcd(t->tm_year % 100);
    }
}

static void rtc_write(ChihiroUsbBoard *b)
{
    struct tm date = {
        .tm_sec = unbcd(b->rtc[0]), .tm_min = unbcd(b->rtc[1]), .tm_hour = unbcd(b->rtc[2]),
        .tm_mday = unbcd(b->rtc[4]), .tm_mon = unbcd(b->rtc[5]) - 1,
        .tm_year = unbcd(b->rtc[6]) + 100, .tm_isdst = -1,
    };
    const time_t at = mktime(&date);
    if (at != (time_t)-1) {
        b->rtc_offset = (int64_t)at - time(NULL);
    }
}

/* JVS frames pass the QC unescaped both ways: its firmware escapes only on
 * the RS-485 line. An escaped reply fails the game's checksum (Error 11). */
static void jvs_send(ChihiroUsbBoard *b, unsigned wire_len)
{
    uint8_t result[260];
    const uint8_t node = b->jvs.device_id;
    const uint8_t *frame = b->jvs_tx + JVS_HEADER_BYTES;
    unsigned n = JVS_REPLY_HEADER_BYTES;
    uint8_t *reply = b->jvs_reply;
    if (wire_len >= 4 && frame[0] == NC_JVS_SYNC && wire_len == 3u + frame[2]) {
        const int got = nc_jvs_frame(&b->jvs, frame, (int)wire_len, result, sizeof(result));
        static unsigned budget = 2000;
        if (tracing(&budget)) {
            char h[73], r[73];
            nc_log("xbased-chihiro", "jvs:%s ->%s", hex(frame, wire_len, h),
                   got > 0 ? hex(result, (unsigned)got, r) : " (none)");
        }
        if (got > 0 && n + (unsigned)got <= sizeof(b->jvs_reply)) {
            memcpy(reply + n, result, (size_t)got);
            n += (unsigned)got;
        }
    }
    reply[0] = b->jvs_tx[0];
    reply[1] = b->jvs_tx[1];
    reply[2] = node;
    reply[3] = 0;
    nc_wr16(reply + 4, (uint16_t)(n - JVS_REPLY_HEADER_BYTES));
    b->jvs_reply_len = n;
}

static bool qc_request(ChihiroUsbBoard *b, uint8_t request, unsigned offset, unsigned count, uint8_t reply[8])
{
    switch (request) {
    case 0x15:                  /* QC status and JVS sense */
        reply[0] = 0x99;
        return true;
    case 0x17:                  /* settings EEPROM read */
        return queue_bytes(&b->in[EP_SETTINGS_OR_CARD], b->settings, sizeof(b->settings), offset, count);
    case 0x18:                  /* backup SRAM read */
        return queue_bytes(&b->in[EP_BACKUP_OR_DRIVE], b->sram, sizeof(b->sram), offset, count);
    case 0x19:                  /* JVS receive */
        if (!b->jvs_reply_len) {
            memset(b->jvs_reply, 0, JVS_REPLY_HEADER_BYTES);
            b->jvs_reply[1] = 1;
            b->jvs_reply_len = JVS_REPLY_HEADER_BYTES;
            static unsigned budget = 50;
            if (tracing(&budget)) {
                nc_log("xbased-chihiro", "jvs: receive with no reply yet");
            }
        }
        b->in[EP_JVS] = (ChihiroUsbTransfer){b->jvs_reply, b->jvs_reply_len};
        nc_wr16(reply + 4, (uint16_t)b->jvs_reply_len);
        return true;
    case 0x1c:                  /* RTC read */
        rtc_read(b);
        return queue_bytes(&b->in[EP_RTC], b->rtc, sizeof(b->rtc), 0, count);
    case 0x1e:                  /* settings EEPROM write */
        return queue_bytes(&b->out[EP_SETTINGS_OR_CARD], b->settings, sizeof(b->settings), offset, count);
    case 0x1f:                  /* backup SRAM write */
        return queue_bytes(&b->out[EP_BACKUP_OR_DRIVE], b->sram, sizeof(b->sram), offset, count);
    case 0x20:                  /* JVS transmit */
        if (count < JVS_HEADER_BYTES || count > sizeof(b->jvs_tx)) {
            return false;
        }
        b->jvs_tx_len = count;
        b->jvs_reply_len = 0;
        return queue_bytes(&b->out[EP_JVS], b->jvs_tx, sizeof(b->jvs_tx), 0, count);
    case 0x24:                  /* RTC write */
        return queue_bytes(&b->out[EP_RTC], b->rtc, sizeof(b->rtc), 0, count);
    default:
        return false;
    }
}

/* 8N1 at 38,400 baud (CRP-1231) and 31,250 baud (V257 MIDI). */
static uint64_t uart_byte_ns(unsigned channel)
{
    return 10000000000ull / (channel ? 31250u : 38400u);
}

static void uart_receive(ChihiroUsbBoard *b, unsigned channel, unsigned count, uint8_t reply[8])
{
    ChihiroUart *uart = &b->uart[channel];
    const unsigned ep = EP_SETTINGS_OR_CARD + channel;
    ChihiroUsbTransfer *transfer = &b->in[ep];
    if (transfer->left) {
        /* The endpoint still owns the bytes advertised by the previous
         * control request. A poll or retry must leave that packet intact. */
        nc_wr16(reply + 4, (uint16_t)MIN(count, transfer->left));
        return;
    }
    const uint64_t now = nc_ke_now_ns();
    unsigned n = 0;
    if (now >= uart->tx_until_ns) {
        for (; n < MIN(count, CHIHIRO_USB_PACKET) && now >= uart->rx_next_ns; n++) {
            const bool got = channel ? nc_v257_tx(&nc_chihiro_v257, &b->rx[channel][n])
                                     : nc_card_reader_tx(&nc_chihiro_card_reader, &b->rx[channel][n]);
            if (!got) {
                break;
            }
            uart->rx_next_ns += uart_byte_ns(channel);
        }
    }
    *transfer = (ChihiroUsbTransfer){b->rx[channel], n};
    nc_wr16(reply + 4, (uint16_t)n);
    static unsigned budget = 300;
    if (n && !channel && tracing(&budget)) {
        char h[73];
        nc_log("xbased-chihiro", "card <-%s", hex(b->rx[channel], n, h));
    }
}

static bool sc_request(ChihiroUsbBoard *b, uint8_t request, unsigned value, unsigned count, uint8_t reply[8])
{
    switch (request) {
    case 0x1a:                  /* UART receive */
    case 0x1b:
        uart_receive(b, request - 0x1a, count, reply);
        return true;
    case 0x22:                  /* UART transmit: the bytes follow on bulk OUT */
    case 0x23:
        b->out[EP_SETTINGS_OR_CARD + request - 0x22].left = count;
        return true;
    case 0x31:                  /* port B */
        b->port_b = (uint8_t)value;
        return true;
    default:
        if (request >= SC_UART_FIRST && request <= SC_UART_LAST && (value >> 8) < 2) {
            b->uart[value >> 8].config[request - SC_UART_FIRST] = (uint8_t)value;
            return true;
        }
        return false;
    }
}

static int board_control(NcUsbDev *dev, const uint8_t setup[8], uint8_t *data, int len)
{
    ChihiroUsbBoard *b = dev->opaque;
    const uint16_t value = nc_rd16(setup + 2);
    const uint16_t count = nc_rd16(setup + 4);
    const uint8_t request = setup[1];
    static unsigned budget = 180;
    if (tracing(&budget)) {
        nc_log("xbased-chihiro", "%s: request %02x:%02x value %04x index %04x length %d",
               dev->name, setup[0], request, value, count, len);
    }
    if (setup[0] == 0x40 && request == REQ_ANCHOR_LOAD && value == CPU_CONTROL && len > 0) {
        b->cpu_held = (data[0] & 1) != 0;
        if (b->cpu_held) {
            reset_transfers(b);
        }
        return 0;
    }
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
        /* status prefix: JVS board unaddressed (53) or addressed (52) */
        reply[1] = 0xcb;
        reply[2] = b->jvs.sense ? 0x53 : 0x52;
        reply[3] = b->jvs.sense ? 0 : 0x10;
    } else {
        /* the SC has no JVS bus and always answers 53 (52 gives Error 11) */
        reply[2] = 0x53;
        if (request != 0x16) {
            reply[7] = (b->uart[0].config[SC_UART_CONTROL - SC_UART_FIRST] & 1) << 6;
        }
    }
    bool handled;
    if (request == 0x16 || (b->qc && request == 0x1d)) {
        ChihiroUsbTransfer *x = request == 0x16 ? b->in : b->out;
        handled = queue_bytes(&x[EP_FIRMWARE_EEPROM], b->eeprom, sizeof(b->eeprom), value, count);
    } else {
        handled = b->qc ? qc_request(b, request, value, count, reply) : sc_request(b, request, value, count, reply);
    }
    if (!handled) {
        static unsigned told;
        if (told++ < 40) {
            nc_log("xbased-chihiro", "%s: unsupported request %02x value %04x count %04x", dev->name, request, value, count);
        }
        return NC_USB_STALL;
    }
    static unsigned sc_budget = 400;
    if (!b->qc && tracing(&sc_budget)) {
        nc_log("xbased-chihiro", "sc: %02x v%04x i%04x -> %02x %02x %02x %02x %02x %02x %02x %02x", request, value, count,
               reply[0], reply[1], reply[2], reply[3], reply[4], reply[5], reply[6], reply[7]);
    }
    const int n = MIN(len, (int)sizeof(reply));
    memcpy(data, reply, (size_t)n);
    return n;
}

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
        const unsigned channel = (unsigned)ep - EP_SETTINGS_OR_CARD;
        ChihiroUart *uart = &b->uart[channel];
        const uint64_t now = nc_ke_now_ns();
        if (uart->tx_until_ns < now) {
            uart->tx_until_ns = now;
        }
        uart->tx_until_ns += uart_byte_ns(channel) * n;
        uart->rx_next_ns = uart->tx_until_ns + uart_byte_ns(channel);
        for (unsigned i = 0; i < n; i++) {
            if (channel) {
                nc_v257_rx(&nc_chihiro_v257, bytes[i]);
            } else {
                nc_card_reader_rx(&nc_chihiro_card_reader, bytes[i]);
            }
        }
        static unsigned budget = 300;
        if (!channel && tracing(&budget)) {
            char h[73];
            nc_log("xbased-chihiro", "card ->%s", hex(bytes, n, h));
        }
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
        if (ep == EP_JVS) {
            jvs_send(b, b->jvs_tx_len - JVS_HEADER_BYTES);
        } else if (ep == EP_RTC) {
            rtc_write(b);
        }
    }
    return (int)n;
}

enum {
    ACBU_SLOT_SIZE = 64,
    ACBU_HEADER_SIZE = 12,
    ACBU_COIN_OFFSET = 0x1c,
    COIN_RATE = 6,
    COIN_FREE_PLAY = 7,
    COIN_CREDITS = 8,
    COIN_SECOND_CHUTE_CREDITS = 9,
    COIN_BONUS = 10,
    COIN_DIVISOR = 11,
    COIN_COST = 12,
};

static uint16_t acbu_checksum(const uint8_t *slot)
{
    uint16_t sum = 0;
    for (unsigned i = ACBU_HEADER_SIZE; i < ACBU_SLOT_SIZE; i++) {
        sum += slot[i];
    }
    return sum;
}

static void init_settings(ChihiroUsbBoard *b)
{
    static const uint8_t coin_settings[] = {
        99, 9, 4, 0, 0, 0, 10, 1, 1, 1, 0, 1, 1, 1, 1, 1, 1, 1, 1, 1,
    };
    uint8_t *s = b->settings;
    memcpy(s, "ACBU0001", 8);
    nc_wr16(s + 8, (ACBU_SLOT_SIZE - ACBU_HEADER_SIZE) / 4);
    s[ACBU_HEADER_SIZE] = s[ACBU_HEADER_SIZE + 1] = 1;
    memcpy(s + ACBU_COIN_OFFSET, coin_settings, sizeof(coin_settings));
    nc_wr16(s + 10, acbu_checksum(s));
    memcpy(s + ACBU_SLOT_SIZE, s, ACBU_SLOT_SIZE);
}

void chihiro_usb_apply_coins(ChihiroUsbBoard *b, const NcChihiroCoinSettings *coins)
{
    for (unsigned offset = 0; offset < 2 * ACBU_SLOT_SIZE; offset += ACBU_SLOT_SIZE) {
        uint8_t *slot = b->settings + offset;
        if (memcmp(slot, "ACBU0001", 8) || nc_rd16(slot + 8) != (ACBU_SLOT_SIZE - ACBU_HEADER_SIZE) / 4 ||
            nc_rd16(slot + 10) != acbu_checksum(slot)) {
            continue;
        }
        uint8_t *stored = slot + ACBU_COIN_OFFSET;
        uint8_t next[COIN_COST + NC_CH_COST_COUNT];
        memcpy(next, stored, sizeof(next));
        const unsigned rate = coins->rate;
        if (rate >= 1 && rate <= 10) {
            /* WMMT acLib validates both the rate index and its derived fields. */
            next[COIN_RATE] = rate;
            next[COIN_FREE_PLAY] = rate == 10;
            next[COIN_CREDITS] = rate >= 6 && rate <= 9 ? rate - 4 : 1;
            next[COIN_SECOND_CHUTE_CREDITS] = 1;
            next[COIN_BONUS] = 0;
            next[COIN_DIVISOR] = rate <= 5 ? rate : 1;
        }
        for (int i = 0; i < NC_CH_COST_COUNT; i++) {
            if (coins->cost[i] >= 1 && coins->cost[i] <= 9) {
                next[COIN_COST + i] = coins->cost[i];
            }
        }
        if (memcmp(next, stored, sizeof(next))) {
            memcpy(stored, next, sizeof(next));
            nc_wr16(slot + 10, acbu_checksum(slot));
            b->backup_dirty = true;
        }
    }
}

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
    memset(b->eeprom, 0xff, sizeof(b->eeprom));
    const uint8_t *desc = b->usb.dev_desc;
    b->eeprom[0] = 0xb2;
    memcpy(b->eeprom + 1, desc + 8, 6);   /* VID, PID and release in the EEPROM header */
    if (qc) {
        b->eeprom[0x1f00] = 1;           /* cabinet region: Japan, then its complement */
        b->eeprom[0x1f01] = (uint8_t)~b->eeprom[0x1f00];
        nc_jvs_init(&b->jvs);
        init_settings(b);
    }
}

