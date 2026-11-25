/*
 * xbased native core — the Chihiro baseboard, media board and network board.
 * The QC (JVS, backup memory) and SC (serial devices) sit on the first USB
 * controller's root ports in place of the Xbox controllers.
 */
#ifndef NATIVE_DEV_CHIHIRO_BOARD_H
#define NATIVE_DEV_CHIHIRO_BOARD_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

struct NcUsbDev;

typedef enum NcChihiroCost {
    NC_CH_COST_START,
    NC_CH_COST_CONTINUE,
    NC_CH_COST_NEW_CARD,
    NC_CH_COST_FULL_COURSE,
    NC_CH_COST_RENEW_CARD,
    NC_CH_COST_TRANSFER_CARD,
    NC_CH_COST_COUNT,
} NcChihiroCost;

typedef struct NcChihiroCoinSettings {
    uint8_t rate;                       /* 0: keep the board setting */
    uint8_t cost[NC_CH_COST_COUNT];      /* 0: keep the board setting */
} NcChihiroCoinSettings;

/* dir holds the cabinet's card files. save holds its settings and backup
 * SRAM across runs (NULL: keep them in memory for this run). */
void nc_chihiro_board_init(const char *dir, const char *save, const NcChihiroCoinSettings *coins,
                           unsigned cabinet);
bool nc_chihiro_board_active(void);
unsigned nc_chihiro_link_cabinet(void);
bool nc_chihiro_link_firmware(const char *dir, char *path, size_t capacity);

/* Card slot 0 selects the latest available card. */
void nc_chihiro_card_select(int slot);
int nc_chihiro_card_selected(void);
int nc_chihiro_card_step(int slot, int direction);
struct NcUsbDev *nc_chihiro_board_qc(void);
struct NcUsbDev *nc_chihiro_board_sc(void);

/* Each 1 ms USB frame (OHCI worker). */
void nc_chihiro_board_frame(void);

/* Writes the battery-backed memory to the save file if it changed. */
void nc_chihiro_board_flush(void);

/* The cabinet's controls, from the host's input. */
typedef struct NcChihiroInput {
    uint16_t steering;      /* 0x0000 left .. 0x8000 centre .. 0xFFFF right */
    uint16_t gas, brake;    /* 0 released .. 0xFFFF pressed */
    bool test, service, coin, start, view, perspective;
    bool shift_up, shift_down;
    bool h_shifter;         /* the gear buttons select the gear directly */
    uint8_t gear;           /* 0 neutral, 1–6 with h_shifter */
    bool card;              /* inserts or takes back the card */
} NcChihiroInput;
void nc_chihiro_board_input(const NcChihiroInput *in);
/* A host wheel: with axes its steering and pedals
 * replace the controller's; its buttons add to the controller's. */
void nc_chihiro_board_wheel(bool axes, const NcChihiroInput *in);
/* TEST pressed within the last 3 s: reboot into the test program */
bool nc_chihiro_board_test_pressed(void);

/* The steering board's motor command in cabinet coordinates. */
typedef struct NcChihiroForce {
    uint16_t target;        /* 0 left .. 0x3FF right */
    uint8_t spring, damping; /* 0 .. 0x7F */
    int8_t push;            /* -0x7F .. 0x7F, positive toward the right */
} NcChihiroForce;

/* On the OHCI worker or under its lock. False with the motor off. */
bool nc_chihiro_board_force(NcChihiroForce *force);

/* The media board's serial is based on the emulated console's identity. */
void nc_chihiro_media_init(const char serial[12]);
bool nc_chihiro_media_exchange(unsigned dimm_factor, const uint32_t command[8], uint32_t reply[8]);
bool nc_chihiro_net_exchange(unsigned dimm_factor, const uint32_t command[8], uint32_t reply[8],
                             uint32_t *delay_ms);
bool nc_chihiro_net_window(uint32_t off, void *buf, uint32_t len, bool write);

/* The media board's LPC ports 0x4000-0x40FF (chihiro_lpc.c). */
void nc_chihiro_lpc_reset(unsigned dimm_factor);
bool nc_chihiro_lpc_in(uint16_t port, unsigned bytes, uint32_t *value);
bool nc_chihiro_lpc_out(uint16_t port, unsigned bytes, uint32_t value);
/* Link play: the network board runs firmware (ver1305.bin) as cabinet 1-4. */
bool nc_chihiro_lpc_load_network(const char *firmware, const char serial[16], unsigned cabinet);
void nc_chihiro_lpc_attach_network(void);
bool nc_chihiro_lpc_link_active(void);
/* Each 1 ms: raises IRQ 10 when a board message is due */
void nc_chihiro_lpc_frame(void);

#ifdef __cplusplus
}
#endif

#endif
