/*
 * Chihiro media board ports: state shared by lpc.c and lpc_net.c
 * (internal). Everything here is protected by chihiro_lpc_lock.
 */
#ifndef NATIVE_DEV_CHIHIRO_LPC_INT_H
#define NATIVE_DEV_CHIHIRO_LPC_INT_H

#include <pthread.h>
#include <stdbool.h>
#include <stdint.h>

#define MEDIA_IRQ 10                /* the media board's interrupt line */
#define NET_QUEUE_SIZE 8            /* Net DIMM replies waiting to go out */
#define NET_SECTOR_SIZE 512
#define NET_WINDOW_SIZE (1u << 20)  /* memory shared with the Net DIMM: 1 MB */

/* The Net DIMM's shared memory. The port word streams (lpc_net.c) and the
 * kernel's mbcom sector reads (nc_chihiro_net_window) see the same bytes. */
typedef struct NetMemory {
    uint32_t response[8];           /* the reply the game reads next */
    uint8_t response_data[NET_SECTOR_SIZE - 32];
    uint32_t command[8];            /* the command the game wrote */
    uint8_t command_data[NET_WINDOW_SIZE - NET_SECTOR_SIZE - 32];
} NetMemory;

_Static_assert(sizeof(NetMemory) == NET_WINDOW_SIZE, "Net DIMM window size");

/* A built-in Net DIMM reply, sent once due_ns has passed (0: free slot). */
typedef struct NetCompletion {
    uint64_t due_ns;
    uint32_t reply[8];
} NetCompletion;

typedef struct ChihiroLpc {
    uint32_t reg_addr;              /* port 0x4004: which register 0x4000 reads/writes */
    uint32_t saddr;                 /* SADDR pointer into the board, auto-incremented */
    uint32_t dma_status;
    bool dma_active;
    uint32_t asic_cpu;              /* 1 once SEGABOOT has started the board's CPU */
    uint32_t cmd[8];                /* DIMM mailbox: the game's command */
    uint32_t resp[8];               /* DIMM mailbox: the board's reply */
    uint16_t scratch;
    uint8_t e0;                     /* port 0x40E0: bit 0 DIMM reply, bit 2 Net DIMM reply */
    bool e1_armed;
    unsigned dimm_factor;           /* 0 128 MB .. 3 1 GB */
    uint64_t raise_at_ns;           /* when to raise IRQ 10 for resp (0: nothing pending) */
    NetMemory net;
    unsigned net_cmd_i, net_resp_i; /* word positions in the Net DIMM streams */
    NetCompletion net_queue[NET_QUEUE_SIZE];
} ChihiroLpc;

extern ChihiroLpc chihiro_lpc;
extern pthread_mutex_t chihiro_lpc_lock;

uint64_t nc_ke_now_ns(void);

/* lpc.c: NC_TRACE_CHIHIRO logging; fmt gets a, b and a running count. */
void chihiro_lpc_trace(const char *fmt, uint32_t a, uint32_t b);
/* lpc.c: logs (a limited number of) accesses nothing handles. */
void chihiro_lpc_unknown(const char *what, uint32_t a, uint32_t v);
/* lpc.c: drops and raises IRQ 10 so the game sees a new edge. */
void chihiro_lpc_irq_pulse(void);

/* lpc_net.c: the Net DIMM registers. False if reg isn't one of them. */
bool chihiro_lpc_net_read(uint32_t reg, uint32_t *value);
bool chihiro_lpc_net_write(uint32_t reg, uint32_t value);
/* lpc_net.c: each 1 ms, sends a built-in reply that is due. */
void chihiro_lpc_net_frame(uint64_t now);
/* lpc_net.c: the game cleared these bits of port 0x40E0. */
void chihiro_lpc_net_ack(uint32_t bits);

#endif
