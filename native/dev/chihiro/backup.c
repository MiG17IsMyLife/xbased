/*
 * Chihiro backup memory: keeps the QC's settings EEPROM and battery-backed
 * SRAM in a save file between runs (board_int.h).
 *
 * The save file is the 256-byte settings EEPROM followed by the 64 KB SRAM.
 */
#include "board_int.h"
#include "file_util.h"

#include <stdio.h>
#include <string.h>

static char g_save[600];            /* the save file, "" for none */
static ChihiroUsbBoard *g_link_qc;  /* link play: the QC to patch */
static unsigned g_link_cabinet;     /* link play: cabinet number 1-4 */

void chihiro_backup_load(ChihiroUsbBoard *qc, const char *save)
{
    snprintf(g_save, sizeof(g_save), "%s", save ? save : "");
    if (!g_save[0]) {
        return;
    }
    FILE *f = fopen(g_save, "rb");
    if (!f) {
        return;                     /* no save yet: keep the defaults */
    }
    uint8_t settings[CHIHIRO_SETTINGS_SIZE], sram[CHIHIRO_SRAM_SIZE];
    const bool ok = fread(settings, 1, sizeof(settings), f) == sizeof(settings) &&
                    fread(sram, 1, sizeof(sram), f) == sizeof(sram);
    fclose(f);
    if (ok) {
        memcpy(qc->settings, settings, sizeof(settings));
        memcpy(qc->sram, sram, sizeof(sram));
        nc_log("xbased-chihiro", "board memory from %s", g_save);
    }
}

void chihiro_backup_flush(ChihiroUsbBoard *qc)
{
    if (!g_save[0] || !qc->backup_dirty) {
        return;
    }
    const ChihiroFilePart parts[] = {{qc->settings, sizeof(qc->settings)}, {qc->sram, sizeof(qc->sram)}};
    if (chihiro_file_replace(g_save, parts, 2)) {
        qc->backup_dirty = false;
    }
}

/* ---------------------------------------------------------- link play */

/*
 * Writes the cabinet's PCB ID into Maximum Tune's backup record, as the
 * game's test menu (GAME ASSIGNMENTS) would. The game takes its network
 * address from this ID, so two linked cabinets with the same ID can't
 * see each other.
 *
 * The record starts with "SBKD0000" (Maximum Tune 2) or "SBHQ0000"
 * (Maximum Tune 1) somewhere from SRAM offset 0x8000:
 *   +8   u16  data length in 4-byte words
 *   +12  data: +0 u32 byte sum of data[4..], +0xA the PCB ID (0-based)
 * Layout from Tovarichtch/xemu chihiro-usb.c.
 */
static void link_pcb_id(ChihiroUsbBoard *qc, unsigned cabinet)
{
    static const char *const tags[] = {"SBKD0000", "SBHQ0000"};
    uint8_t *mem = qc->sram;
    for (int t = 0; t < 2; t++) {
        for (size_t pos = 0x8000; pos + 12 <= sizeof(qc->sram); pos++) {
            if (memcmp(mem + pos, tags[t], 8)) {
                continue;
            }
            const size_t size = (size_t)(mem[pos + 8] | mem[pos + 9] << 8) * 4;
            uint8_t *data = mem + pos + 12;
            if (size < 0x10 || pos + 12 + size > sizeof(qc->sram)) {
                return;             /* damaged record: leave it alone */
            }
            if (data[0xA] != cabinet - 1) {
                data[0xA] = (uint8_t)(cabinet - 1);
                uint32_t sum = 0;
                for (size_t i = 4; i < size; i++) {
                    sum += data[i];
                }
                nc_wr32(data, sum);
                qc->backup_dirty = true;
                nc_log("xbased-chihiro", "link: PCB ID %u in the game's backup", cabinet);
            }
            return;
        }
    }
}

/* On a fresh board the game writes its record for the first time while
 * running; patch it then too. The game reads it on its next start. */
static void backup_written(void)
{
    link_pcb_id(g_link_qc, g_link_cabinet);
}

void chihiro_backup_link(ChihiroUsbBoard *qc, unsigned cabinet)
{
    g_link_qc = qc;
    g_link_cabinet = cabinet;
    link_pcb_id(qc, cabinet);
    qc->backup_written = backup_written;
}
