/*
 * xbased native core — the CRP-1231 card reader-printer (crp1231.h).
 * Adapted from Tovarichtch/xemu's chihiro-cardreader-crp1231.c (98fbd75).
 * Copyright (c) 2026 Réda Chérif-Touil. SPDX-License-Identifier: LGPL-2.0-or-later
 *
 * Commands and response digits follow Sanwa's CRW/CRWL driver manual
 * (tables 3-1 to 3-4). Frame: STX, LEN, command, 3 bytes, parameters, ETX,
 * sum (LEN xor command..ETX). The host gets ACK/NAK at once and the answer,
 * STX 06 cmd r1 r2 r3 ETX sum, on each ENQ.
 */
#include "crp1231.h"

#include <stdio.h>
#include <string.h>

uint64_t nc_ke_now_ns(void);

enum { STX = 0x02, ETX = 0x03, ENQ = 0x05, ACK = 0x06, NAK = 0x15 };
enum { WRITE_DATA_OFFSET = 9, FRAME_TRAILER_BYTES = 2 };
enum { NOWHERE, INSIDE, HANDED_BACK, IN_SLOT };    /* where the card is */
enum {
    CMD_TAKE = 0x33,
    CMD_CANCEL = 0x40,
    CMD_WRITE = 0x53,
    CMD_PRINT = 0x7C,
    CMD_RETURN = 0x80,
    CMD_CLEAN = 0xA0,
    CMD_DISPENSE = 0xB0,
};

void nc_card_reader_init(NcCardReader *r)
{
    bool (*save_card)(char *, size_t, const uint8_t *) = r->save_card;
    void (*note)(const char *) = r->note;
    memset(r, 0, sizeof(*r));
    r->save_card = save_card;
    r->note = note;
}

static uint8_t sum(const uint8_t *f, int len)
{
    uint8_t s = f[1];
    for (int i = 2; i <= len - 2; i++) {
        s ^= f[i];
    }
    return s;
}

static void emit(NcCardReader *r, const uint8_t *b, int n)
{
    if (r->out_len + n <= (int)sizeof(r->out)) {
        memcpy(r->out + r->out_len, b, (size_t)n);
        r->out_len += n;
    }
}

static void keep(NcCardReader *r, const uint8_t *f, int n)
{
    memcpy(r->last, f, (size_t)n);
    r->last_len = n;
}

static void gone(NcCardReader *r)
{
    r->where = NOWHERE;
    r->written = false;
    memset(r->card, 0, sizeof(r->card));
    r->path[0] = 0;
}

static void note(NcCardReader *r, const char *what)
{
    if (r->note) {
        r->note(what);
    }
}

/* A card file is either empty (blank) or has a 69-byte stripe. */
static bool load(NcCardReader *r, const char *path)
{
    gone(r);
    if (!path || !*path) {
        return true;
    }
    FILE *f = fopen(path, "rb");
    if (f) {
        uint8_t b[NC_CARD_BYTES + 1];
        const size_t n = fread(b, 1, sizeof(b), f);
        const bool bad = ferror(f) || (n && n != NC_CARD_BYTES);
        fclose(f);
        if (bad) {
            return false;
        }
        if (n) {
            memcpy(r->card, b, NC_CARD_BYTES);
            for (int i = 0; i < NC_CARD_BYTES && !r->written; i++) {
                r->written = r->card[i] != 0;   /* zeros read as a blank stripe */
            }
        }
    } else {
        return false;
    }
    snprintf(r->path, sizeof(r->path), "%s", path);
    return true;
}

/* Cleaning: 1 s taking the card in, 3 s cleaning, 1 s at the slot. */
static void cleaning(NcCardReader *r, uint8_t *s3, uint8_t *s5)
{
    const uint64_t ms = (nc_ke_now_ns() - r->clean_start_ns) / 1000000u;
    if (ms < 1000) {
        *s5 = '4';
    } else if (ms < 4000) {
        *s5 = '3';
    } else if (ms < 5000) {
        *s3 = '4';
    } else {
        r->cleaning = false;
    }
}

static void act(NcCardReader *r, uint8_t cmd, uint8_t param, uint8_t *s3, uint8_t *s4, uint8_t *s5)
{
    const bool in = r->where == INSIDE;
    switch (cmd) {
    case CMD_TAKE:
        if (param != '0' && param != '2') {
            *s5 = '2';
        } else if (r->waiting) {
            *s5 = '4';
        } else if (param == '0' && !r->written) {
            *s4 = '1';                          /* nothing on the stripe */
        }
        break;
    case CMD_CANCEL:
        if (r->waiting || r->cleaning) {
            r->waiting = r->cleaning = false;
        } else {
            *s5 = '2';
        }
        break;
    case CMD_CLEAN:
        if (!r->cleaning) {
            r->cleaning = true;
            r->clean_start_ns = nc_ke_now_ns();
        }
        break;
    case CMD_DISPENSE:
        if (in) {
            *s5 = '2';                          /* one is inside already */
            break;
        }
        if (r->where == IN_SLOT) {
            note(r, "card out");                /* back to the player's hand */
        }
        gone(r);
        r->where = INSIDE;                      /* a blank from the stacker */
        break;
    case CMD_RETURN:
        if (r->where != NOWHERE) {
            r->where = HANDED_BACK;
        }
        break;
    case CMD_WRITE:
    case CMD_PRINT:
        if (!in) {
            *s5 = '2';
        }
        break;
    default:
        break;
    }
    *s3 = r->where == INSIDE ? '1' : r->where == HANDED_BACK ? '4' : '0';
    if (r->cleaning) {
        cleaning(r, s3, s5);
    }
}

static void respond(NcCardReader *r, uint8_t cmd, uint8_t param, const uint8_t *write_data)
{
    if (r->where == HANDED_BACK) {              /* taken by the next question */
        gone(r);
        note(r, "card out");
    }
    r->last_cmd = cmd;
    r->last_param = param;
    if (cmd == CMD_TAKE && (param == '0' || param == '2')) {
        if (r->where == IN_SLOT) {
            r->where = INSIDE;                  /* drawn in */
        }
        r->waiting = r->where == NOWHERE;
    } else if (cmd != CMD_CANCEL) {
        r->waiting = false;
    }
    if (cmd == CMD_TAKE && param == '0' && r->where == INSIDE && r->written) {
        uint8_t f[77] = {STX, 0x4B, CMD_TAKE, '1', '0', '0'};
        memcpy(f + 6, r->card, NC_CARD_BYTES);
        f[75] = ETX;
        f[76] = sum(f, sizeof(f));
        keep(r, f, sizeof(f));
        return;
    }
    uint8_t f[8] = {STX, 0x06, cmd, '0', '0', '0', ETX, 0};
    act(r, cmd, param, &f[3], &f[4], &f[5]);
    if (cmd == CMD_WRITE && f[5] == '0') {
        if (!write_data) {
            f[5] = '2';                  /* command cannot be executed */
        } else if (!r->save_card || !r->save_card(r->path, sizeof(r->path), write_data)) {
            f[4] = '2';                  /* write error, Sanwa table 3-3 */
        } else {
            memcpy(r->card, write_data, sizeof(r->card));
            r->written = true;
        }
    }
    f[7] = sum(f, sizeof(f));
    keep(r, f, sizeof(f));
}

bool nc_card_reader_can_offer(const NcCardReader *r)
{
    return r->where == NOWHERE;
}

bool nc_card_reader_offer(NcCardReader *r, const char *path)
{
    if (!nc_card_reader_can_offer(r) || (path && strlen(path) >= sizeof(r->path)) || !load(r, path)) {
        return false;
    }
    r->where = IN_SLOT;
    if (r->waiting) {
        respond(r, r->last_cmd, r->last_param, NULL);  /* drawn in at once */
    }
    return true;
}

bool nc_card_reader_take_back(NcCardReader *r)
{
    if (r->where != IN_SLOT) {
        return false;
    }
    gone(r);
    return true;
}

void nc_card_reader_rx(NcCardReader *r, uint8_t byte)
{
    if (!r->in_len) {
        if (byte == ENQ || byte == NAK) {
            if (!r->last_len || r->out_pos < r->out_len) {
                return;                         /* nothing yet, or still sending */
            }
            /* a card handed back, or a clean, is worked out again */
            if (!r->fresh && (r->where == HANDED_BACK || r->cleaning)) {
                respond(r, r->last_cmd, r->last_param, NULL);
            }
            r->fresh = false;
            r->out_len = r->out_pos = 0;
            emit(r, r->last, r->last_len);
            return;
        }
        if (byte != STX) {
            return;
        }
    }
    if (r->in_len >= (int)sizeof(r->in)) {
        r->in_len = r->in_want = 0;
        return;
    }
    r->in[r->in_len++] = byte;
    if (r->in_len == 2) {
        r->in_want = byte + 2;
        if (r->in_want < 4 || r->in_want > (int)sizeof(r->in)) {
            r->in_len = r->in_want = 0;
        }
        return;
    }
    if (!r->in_want || r->in_len < r->in_want) {
        return;
    }
    const int len = r->in_want;
    r->in_len = r->in_want = 0;
    if (r->in[len - 2] != ETX || r->in[len - 1] != sum(r->in, len)) {
        const uint8_t nak = NAK;
        emit(r, &nak, 1);
        return;
    }
    const uint8_t cmd = r->in[2];
    r->out_len = r->out_pos = 0;
    const uint8_t ack = ACK;
    emit(r, &ack, 1);
    r->fresh = true;
    const uint8_t *write_data = cmd == CMD_WRITE && len >= WRITE_DATA_OFFSET + NC_CARD_BYTES + FRAME_TRAILER_BYTES
                                  ? r->in + WRITE_DATA_OFFSET : NULL;
    respond(r, cmd, len >= 7 ? r->in[6] : 0, write_data);
}

bool nc_card_reader_tx(NcCardReader *r, uint8_t *byte)
{
    if (r->out_pos >= r->out_len) {
        return false;
    }
    *byte = r->out[r->out_pos++];
    if (r->out_pos >= r->out_len) {
        r->out_len = r->out_pos = 0;
    }
    return true;
}
