/*
 * Chihiro baseboard QC device (baseboard.h): JVS bus, settings EEPROM,
 * backup SRAM and real-time clock.
 *
 * QC vendor requests:
 *   0x15 status          0x16 / 0x1d firmware EEPROM read / write
 *   0x17 / 0x1e settings EEPROM read / write
 *   0x18 / 0x1f backup SRAM read / write
 *   0x20 JVS send        0x19 JVS receive
 *   0x1c / 0x24 clock read / write
 */
#include "baseboard.h"
#include "trace.h"

#include <string.h>
#include <time.h>

/* ---------------------------------------------------------- clock */

/* The clock chip keeps its time in BCD (each decimal digit in 4 bits). */
static unsigned bcd(unsigned val)
{
    return (val / 10) * 16 + val % 10;
}

static int unbcd(uint8_t val)
{
    return (val >> 4) * 10 + (val & 15);
}

/* Fills b->rtc with the host's local time plus the game's offset. */
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

/* The game set the clock: remember how far it is from the host's time,
 * so the host clock itself is never changed. */
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

/* ---------------------------------------------------------- JVS */

/*
 * Passes the frame the game sent (b->jvs_tx) to the JVS I/O board and
 * builds the reply the game will fetch with request 0x19.
 *
 * Frames go through the QC without JVS escape bytes in both directions:
 * the real QC firmware adds and removes them only on the RS-485 cable.
 * An escaped reply would fail the game's checksum (Error 11).
 */
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
        if (chihiro_trace(&budget)) {
            char h[CHIHIRO_HEX_CAP], r[CHIHIRO_HEX_CAP];
            nc_log("xbased-chihiro", "jvs:%s ->%s", chihiro_hex(frame, wire_len, h),
                   got > 0 ? chihiro_hex(result, (unsigned)got, r) : " (none)");
        }
        if (got > 0 && n + (unsigned)got <= sizeof(b->jvs_reply)) {
            memcpy(reply + n, result, (size_t)got);
            n += (unsigned)got;
        }
    }
    /* Reply header: the send's first two header bytes, the node, 0, then
     * the length of the JVS bytes that follow. */
    reply[0] = b->jvs_tx[0];
    reply[1] = b->jvs_tx[1];
    reply[2] = node;
    reply[3] = 0;
    nc_wr16(reply + 4, (uint16_t)(n - JVS_REPLY_HEADER_BYTES));
    b->jvs_reply_len = n;
}

/* ---------------------------------------------------------- requests */

void chihiro_qc_status(const ChihiroUsbBoard *b, uint8_t reply[8])
{
    /* Byte 1: the baseboard's DIP switches (a 0 bit is "on"). 0xcb is a
     * 31 kHz landscape monitor, which SEGABOOT checks against boot.id.
     * Bytes 2-3: 0x53 while the JVS board has no address yet, 0x52 once
     * it has one. */
    reply[1] = 0xcb;
    reply[2] = b->jvs.sense ? 0x53 : 0x52;
    reply[3] = b->jvs.sense ? 0 : 0x10;
}

bool chihiro_qc_request(ChihiroUsbBoard *b, uint8_t request, unsigned offset, unsigned count, uint8_t reply[8])
{
    switch (request) {
    case 0x15:                  /* QC status and JVS sense line */
        reply[0] = 0x99;
        return true;
    case 0x17:                  /* settings EEPROM read */
        return chihiro_usb_queue(&b->in[EP_SETTINGS_OR_CARD], b->settings, sizeof(b->settings), offset, count);
    case 0x18:                  /* backup SRAM read */
        return chihiro_usb_queue(&b->in[EP_BACKUP_OR_DRIVE], b->sram, sizeof(b->sram), offset, count);
    case 0x19:                  /* JVS receive */
        if (!b->jvs_reply_len) {
            /* No reply yet: an empty one with byte 1 set. */
            memset(b->jvs_reply, 0, JVS_REPLY_HEADER_BYTES);
            b->jvs_reply[1] = 1;
            b->jvs_reply_len = JVS_REPLY_HEADER_BYTES;
            static unsigned budget = 50;
            if (chihiro_trace(&budget)) {
                nc_log("xbased-chihiro", "jvs: receive with no reply yet");
            }
        }
        b->in[EP_JVS] = (ChihiroUsbTransfer){b->jvs_reply, b->jvs_reply_len};
        nc_wr16(reply + 4, (uint16_t)b->jvs_reply_len);
        return true;
    case 0x1c:                  /* clock read */
        rtc_read(b);
        return chihiro_usb_queue(&b->in[EP_RTC], b->rtc, sizeof(b->rtc), 0, count);
    case 0x1e:                  /* settings EEPROM write */
        return chihiro_usb_queue(&b->out[EP_SETTINGS_OR_CARD], b->settings, sizeof(b->settings), offset, count);
    case 0x1f:                  /* backup SRAM write */
        return chihiro_usb_queue(&b->out[EP_BACKUP_OR_DRIVE], b->sram, sizeof(b->sram), offset, count);
    case 0x20:                  /* JVS send: the frame follows on bulk OUT */
        if (count < JVS_HEADER_BYTES || count > sizeof(b->jvs_tx)) {
            return false;
        }
        b->jvs_tx_len = count;
        b->jvs_reply_len = 0;
        return chihiro_usb_queue(&b->out[EP_JVS], b->jvs_tx, sizeof(b->jvs_tx), 0, count);
    case 0x24:                  /* clock write */
        return chihiro_usb_queue(&b->out[EP_RTC], b->rtc, sizeof(b->rtc), 0, count);
    default:
        return false;
    }
}

/* The game finished sending everything a request queued on endpoint ep. */
void chihiro_qc_out_done(ChihiroUsbBoard *b, int ep)
{
    if (ep == EP_JVS) {
        jvs_send(b, b->jvs_tx_len - JVS_HEADER_BYTES);
    } else if (ep == EP_RTC) {
        rtc_write(b);
    } else if (ep == EP_BACKUP_OR_DRIVE && b->backup_written) {
        b->backup_written();
    }
}
