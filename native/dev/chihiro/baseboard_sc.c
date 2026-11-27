/*
 * Chihiro baseboard SC device (baseboard.h): two serial ports (UARTs).
 *
 *   channel 0  card reader (crp1231.c), 38,400 baud
 *   channel 1  the game's drive board (drive.h), 31,250 baud
 *
 * SC vendor requests:
 *   0x1a / 0x1b  receive from channel 0 / 1
 *   0x22 / 0x23  send to channel 0 / 1 (the bytes follow on bulk OUT)
 *   0x25-0x2f    UART configuration (value's high byte is the channel)
 *   0x31         set port B
 *
 * Bytes are paced at the real line speed, because the games time out or
 * lose frames when answers come back faster than real hardware could
 * send them.
 */
#include "baseboard.h"
#include "trace.h"

#include <string.h>

uint64_t nc_ke_now_ns(void);

/* Time one byte takes on the line: 8 data bits + start + stop = 10 bits. */
static uint64_t uart_byte_ns(unsigned channel)
{
    return 10000000000ull / (channel ? 31250u : 38400u);
}

/* Request 0x1a/0x1b: gathers the bytes the device on this channel has
 * ready, up to one packet, for the game to fetch on bulk IN. */
static void uart_receive(ChihiroUsbBoard *b, unsigned channel, unsigned count, uint8_t reply[8])
{
    ChihiroUart *uart = &b->uart[channel];
    const unsigned ep = EP_SETTINGS_OR_CARD + channel;
    ChihiroUsbTransfer *transfer = &b->in[ep];
    if (transfer->left) {
        /* The game hasn't fetched the last packet yet. A repeated request
         * must report the same bytes, not replace them. */
        nc_wr16(reply + 4, (uint16_t)MIN(count, transfer->left));
        return;
    }
    const uint64_t now = nc_ke_now_ns();
    unsigned n = 0;
    if (now >= uart->tx_until_ns && (channel || now >= uart->rx_next_ns)) {
        /*
         * The card reader's answer is handed over whole (up to a packet),
         * not a byte at a time.
         *
         * Why: Maximum Tune (V307.xbe at 0x00072F70) keeps a count of the
         * bytes of a frame split across polls and never clears it when it
         * sends the next command. Once that count is off, a split frame is
         * never put back together. With byte-by-byte pacing an answer
         * could be split behind the tail of an earlier one, get lost, and
         * the game then decided the reader was unplugged (Error 51).
         *
         * The drive board's bytes are still paced one at a time.
         */
        for (; n < MIN(count, CHIHIRO_USB_PACKET) && (!channel || now >= uart->rx_next_ns); n++) {
            const bool got = channel ? nc_chihiro_drive && nc_chihiro_drive->tx &&
                                           nc_chihiro_drive->tx(&b->rx[channel][n])
                                     : nc_card_reader_tx(&nc_chihiro_card_reader, &b->rx[channel][n]);
            if (!got) {
                break;
            }
            if (channel) {
                uart->rx_next_ns += uart_byte_ns(channel);
            }
        }
        if (!channel && n) {
            /* The card reader's next answer waits until these bytes would
             * have finished arriving. */
            uart->rx_next_ns = now + uart_byte_ns(channel) * n;
        }
    }
    *transfer = (ChihiroUsbTransfer){b->rx[channel], n};
    nc_wr16(reply + 4, (uint16_t)n);
    static unsigned budget = 300;
    if (n && !channel && chihiro_trace(&budget)) {
        char h[CHIHIRO_HEX_CAP];
        nc_log("xbased-chihiro", "card <-%s", chihiro_hex(b->rx[channel], n, h));
    }}

/* Bulk OUT on a UART endpoint: hands the bytes to the device on that
 * channel and works out when they finish going out on the line. */
void chihiro_sc_send(ChihiroUsbBoard *b, unsigned channel, const uint8_t *bytes, unsigned n)
{
    ChihiroUart *uart = &b->uart[channel];
    const uint64_t now = nc_ke_now_ns();
    if (uart->tx_until_ns < now) {
        uart->tx_until_ns = now;
    }
    uart->tx_until_ns += uart_byte_ns(channel) * n;
    /* Nothing comes back until one byte time after the send ends. */
    uart->rx_next_ns = uart->tx_until_ns + uart_byte_ns(channel);
    for (unsigned i = 0; i < n; i++) {
        if (channel) {
            if (nc_chihiro_drive && nc_chihiro_drive->rx) {
                nc_chihiro_drive->rx(bytes[i]);
            }
        } else {
            nc_card_reader_rx(&nc_chihiro_card_reader, bytes[i]);
        }
    }
    static unsigned budget = 300;
    if (!channel && chihiro_trace(&budget)) {
        char h[CHIHIRO_HEX_CAP];
        nc_log("xbased-chihiro", "card ->%s", chihiro_hex(bytes, n, h));
    }}

void chihiro_sc_status(const ChihiroUsbBoard *b, uint8_t request, uint8_t reply[8])
{
    /* The SC has no JVS bus, so it always reports "no address" (0x53);
     * 0x52 there makes the game report Error 11. Byte 7 bit 6 mirrors
     * bit 0 of the card reader UART's control register. */
    reply[2] = 0x53;
    if (request != 0x16) {
        reply[7] = (b->uart[0].config[SC_UART_CONTROL - SC_UART_FIRST] & 1) << 6;
    }
}

bool chihiro_sc_request(ChihiroUsbBoard *b, uint8_t request, unsigned value, unsigned count, uint8_t reply[8])
{
    switch (request) {
    case 0x1a:                  /* UART receive */
    case 0x1b:
        uart_receive(b, request - 0x1a, count, reply);
        return true;
    case 0x22:                  /* UART send: the bytes follow on bulk OUT */
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
