/*
 * Chihiro card reader: Sanwa CRP-1231 (crp1231.h).
 * Adapted from Tovarichtch/xemu's chihiro-cardreader-crp1231.c (98fbd75).
 * Copyright (c) 2026 Réda Chérif-Touil. SPDX-License-Identifier: LGPL-2.0-or-later
 *
 * Protocol (Sanwa's CRW/CRWL driver manual, tables 3-1 to 3-4):
 *
 *   game -> reader  STX, LEN, command, 3 bytes, parameters, ETX, sum
 *                   (sum = LEN xor every byte from command to ETX)
 *   reader -> game  ACK (frame good) or NAK (bad) right away
 *   game -> reader  ENQ: "what's the result?"
 *   reader -> game  STX 06 command r1 r2 r3 ETX sum
 *
 * r1-r3 are ASCII status digits: r1 where the card is, r2 the stripe
 * (read/write errors), r3 the command's own status (busy, can't do it).
 */
#include "crp1231.h"

#include <stdio.h>
#include <string.h>

uint64_t nc_ke_now_ns(void);

enum { STX = 0x02, ETX = 0x03, ENQ = 0x05, ACK = 0x06, NAK = 0x15 };
enum { WRITE_DATA_OFFSET = 9, FRAME_TRAILER_BYTES = 2 };
enum { NOWHERE, INSIDE, HANDED_BACK, IN_SLOT };    /* NcCardReader.where */
enum {
    CMD_TAKE = 0x33,            /* draw in a card (and read it, parameter '0') */
    CMD_CANCEL = 0x40,          /* stop waiting or cleaning */
    CMD_WRITE = 0x53,           /* write the stripe */
    CMD_PRINT = 0x7C,           /* print on the card's face (nothing to do here) */
    CMD_RETURN = 0x80,          /* hand the card back to the player */
    CMD_CLEAN = 0xA0,           /* run a cleaning card through */
    CMD_DISPENSE = 0xB0,        /* take a new blank card from the stack */
};

void nc_card_reader_init(NcCardReader *r)
{
    bool (*save_card)(char *, size_t, const uint8_t *) = r->save_card;
    void (*note)(const char *) = r->note;
    memset(r, 0, sizeof(*r));
    r->save_card = save_card;
    r->note = note;
}

/* The checksum of frame f (len bytes): LEN xor ... xor ETX. */
static uint8_t sum(const uint8_t *f, int len)
{
    uint8_t s = f[1];
    for (int i = 2; i <= len - 2; i++) {
        s ^= f[i];
    }
    return s;
}

/* Queues bytes for the game. */
static void emit(NcCardReader *r, const uint8_t *b, int n)
{
    if (r->out_len + n <= (int)sizeof(r->out)) {
        memcpy(r->out + r->out_len, b, (size_t)n);
        r->out_len += n;
    }
}

/* Stores the answer the next ENQ sends. */
static void keep(NcCardReader *r, const uint8_t *f, int n)
{
    memcpy(r->last, f, (size_t)n);
    r->last_len = n;
}

/* The card has left the reader. */
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

/* Reads a card file: empty (a blank card) or a 69-byte stripe. "" or
 * NULL is a blank card with no file yet. */
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
                r->written = r->card[i] != 0;   /* all zeros counts as blank */
            }
        }
    } else {
        return false;
    }
    snprintf(r->path, sizeof(r->path), "%s", path);
    return true;
}

/* A cleaning cycle's status digits over time: 1 s drawing the cleaning
 * card in, 3 s cleaning, 1 s pushing it out to the slot. */
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

/* Carries out a command and sets the status digits r1-r3 (s3, s4, s5 are
 * their places in the answer frame). '0' everywhere means "done". */
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
            *s4 = '1';                          /* read error: the stripe is blank */
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
            *s5 = '2';                          /* can't: a card is inside already */
            break;
        }
        if (r->where == IN_SLOT) {
            note(r, "card out");                /* the player's card goes back to them */
        }
        gone(r);
        r->where = INSIDE;                      /* now a blank card is inside */
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

/* Works out the answer to a command and stores it for the next ENQ.
 * write_data: the stripe to write (CMD_WRITE), or NULL. */
static void respond(NcCardReader *r, uint8_t cmd, uint8_t param, const uint8_t *write_data)
{
    /* A handed-back card counts as taken once the next command comes. */
    if (r->where == HANDED_BACK) {
        gone(r);
        note(r, "card out");
    }
    r->last_cmd = cmd;
    r->last_param = param;
    if (cmd == CMD_TAKE && (param == '0' || param == '2')) {
        if (r->where == IN_SLOT) {
            r->where = INSIDE;                  /* the card in the slot is drawn in */
        }
        r->waiting = r->where == NOWHERE;
    } else if (cmd != CMD_CANCEL) {
        r->waiting = false;
    }
    /* Reading a written card: the answer carries the 69-byte stripe. */
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
            f[5] = '2';                  /* can't: no data came with it */
        } else if (!r->save_card || !r->save_card(r->path, sizeof(r->path), write_data)) {
            f[4] = '2';                  /* write error (Sanwa table 3-3) */
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
        respond(r, r->last_cmd, r->last_param, NULL);  /* the game was waiting: draw it in now */
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
    /* Between frames: ENQ (or NAK, "send again") asks for the answer;
     * STX starts a new frame; anything else is ignored. */
    if (!r->in_len) {
        if (byte == ENQ || byte == NAK) {
            if (!r->last_len || r->out_pos < r->out_len) {
                return;                         /* no answer yet, or still sending one */
            }
            /* Asked again while a card is handed back or a cleaning runs:
             * work the answer out again, since it changes over time. */
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
        /* The frame's full length: STX and LEN, plus the LEN bytes after them. */
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
