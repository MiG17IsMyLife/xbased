/*
 * xbased native core — the Chihiro baseboard (QC and SC USB devices) and
 * Maximum Tune's cabinet: JVS I/O, card reader, steering board, card files
 * and battery-backed memory.
 */
#include "native/dev/chihiro_board.h"

#include "chihiro_usb.h"
#include "net/chihiro-netboard.h"

#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <pthread.h>
#include <stdatomic.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <time.h>
#include <unistd.h>

#ifdef _WIN32
#include <direct.h>
#include <windows.h>
#define nc_mkdir(p) _mkdir(p)
#else
#define nc_mkdir(p) mkdir((p), 0755)
#endif

NcCardReader nc_chihiro_card_reader;
NcV257 nc_chihiro_v257;

static int64_t host_ns(void)
{
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (int64_t)ts.tv_sec * 1000000000 + ts.tv_nsec;
}

unsigned nc_chihiro_link_cabinet(void)
{
    const char *link = getenv("NC_CHIHIRO_LINK");
    const int cabinet = link ? atoi(link) : 0;
    return cabinet >= 1 && cabinet <= 4 ? (unsigned)cabinet : 0;
}

bool nc_chihiro_link_firmware(const char *dir, char *path, size_t capacity)
{
    const int n = snprintf(path, capacity, "%s/ver1305.bin", dir && *dir ? dir : "chihiro");
    struct stat st;
    return n >= 0 && (size_t)n < capacity && stat(path, &st) == 0 && S_ISREG(st.st_mode) &&
           st.st_size >= CHIHIRO_NETBOARD_MIN_FIRMWARE_BYTES && access(path, R_OK) == 0;
}

bool nc_chihiro_flash_path(const char *dir, char *path, size_t capacity)
{
    static const char *const names[] = {"fpr21042.bin", "fpr21042_m29w160et.bin"};
    for (unsigned i = 0; i < sizeof(names) / sizeof(*names); i++) {
        const int n = snprintf(path, capacity, "%s/%s", dir && *dir ? dir : "chihiro", names[i]);
        if (n >= 0 && (size_t)n < capacity && access(path, R_OK) == 0) {
            return true;
        }
    }
    path[0] = 0;
    return false;
}

/* ------------------------------------------------------------ files */

enum { CARD_SLOT_MAX = 999 };
static char g_cards[512];       /* <dir>/cards */
static char g_save[600];        /* the battery-backed memory's file */
static pthread_mutex_t g_card_lock = PTHREAD_MUTEX_INITIALIZER;
static char g_card_name[NC_CHIHIRO_CARD_NAME_CAP];

static bool valid_card_name(const char *name)
{
    const size_t n = strlen(name);
    if (n > 4 && !strcasecmp(name + n - 4, ".tmp")) {
        return false;               /* one being written (file_replace) */
    }
    return *name && strlen(name) < NC_CHIHIRO_CARD_NAME_CAP &&
           strcmp(name, ".") && strcmp(name, "..") && !strpbrk(name, "/\\:\r\n") &&
           name[strlen(name) - 1] != ' ' && name[strlen(name) - 1] != '\t';
}

static bool card_file(const char *name, char path[NC_CARD_PATH_CAP], struct stat *st)
{
    if (!g_cards[0] || !valid_card_name(name)) {
        return false;
    }
    const int n = snprintf(path, NC_CARD_PATH_CAP, "%s/%s", g_cards, name);
    return n >= 0 && n < NC_CARD_PATH_CAP && stat(path, st) == 0 && S_ISREG(st->st_mode) &&
           (st->st_size == 0 || st->st_size == NC_CARD_BYTES);
}

void nc_chihiro_card_select(const char *name)
{
    pthread_mutex_lock(&g_card_lock);
    snprintf(g_card_name, sizeof(g_card_name), "%s", name && valid_card_name(name) ? name : "");
    pthread_mutex_unlock(&g_card_lock);
}

void nc_chihiro_card_selected(char name[NC_CHIHIRO_CARD_NAME_CAP])
{
    pthread_mutex_lock(&g_card_lock);
    memcpy(name, g_card_name, sizeof(g_card_name));
    pthread_mutex_unlock(&g_card_lock);
}

void nc_chihiro_card_load_setting(const char *value)
{
    if (!value || !*value) {
        nc_chihiro_card_select(NULL);
    } else if (!strncmp(value, "file:", 5)) {
        nc_chihiro_card_select(value + 5);
    } else {
        char *end;
        const long slot = strtol(value, &end, 10);
        char name[24] = "";
        if (!*end && slot >= 1 && slot <= CARD_SLOT_MAX) {
            snprintf(name, sizeof(name), "card%03ld.bin", slot);
        }
        nc_chihiro_card_select(name);
    }
}

static void find_card(char result[NC_CHIHIRO_CARD_NAME_CAP], const char *current, int direction)
{
    result[0] = 0;
    if (!g_cards[0]) {
        return;
    }
#ifdef _WIN32
    wchar_t directory[512];
    if (!MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, g_cards, -1,
                             directory, sizeof(directory) / sizeof(*directory))) {
        return;
    }
    _WDIR *dir = _wopendir(directory);
#else
    DIR *dir = opendir(g_cards);
#endif
    if (!dir) {
        return;
    }
    time_t latest = 0;
#ifdef _WIN32
    for (struct _wdirent *entry; (entry = _wreaddir(dir));) {
        char name[NC_CHIHIRO_CARD_NAME_CAP];
        if (!WideCharToMultiByte(CP_UTF8, 0, entry->d_name, -1, name, sizeof(name), NULL, NULL)) {
            continue;
        }
#else
    for (struct dirent *entry; (entry = readdir(dir));) {
        const char *name = entry->d_name;
#endif
        const int relative = strcmp(name, current);
        const int order = strcmp(name, result);
        if ((direction > 0 && (relative <= 0 || (*result && order >= 0))) ||
            (direction < 0 && ((*current && relative >= 0) || (*result && order <= 0)))) {
            continue;
        }
        char path[NC_CARD_PATH_CAP];
        struct stat st;
        if (card_file(name, path, &st) &&
            (direction || !*result || st.st_mtime > latest || (st.st_mtime == latest && order > 0))) {
            strcpy(result, name);
            latest = st.st_mtime;
        }
    }
#ifdef _WIN32
    _wclosedir(dir);
#else
    closedir(dir);
#endif
}

void nc_chihiro_card_step(char name[NC_CHIHIRO_CARD_NAME_CAP], int direction)
{
    char next[NC_CHIHIRO_CARD_NAME_CAP];
    find_card(next, name, direction);
    strcpy(name, next);
}

typedef struct FilePart {
    const void *data;
    size_t size;
} FilePart;

/* Publish a complete file with a same-directory rename. */
static bool file_replace(const char *path, const FilePart *parts, int count)
{
    char tmp[NC_CARD_PATH_CAP + 32];
    const int n = snprintf(tmp, sizeof(tmp), "%s.%ld.tmp", path, (long)getpid());
    if (n < 0 || (size_t)n >= sizeof(tmp)) {
        return false;
    }
    FILE *f = fopen(tmp, "wb");
    if (!f) {
        nc_log("xbased-chihiro", "can't write %s: %s", tmp, strerror(errno));
        return false;
    }
    bool ok = true;
    for (int i = 0; i < count && ok; i++) {
        ok = fwrite(parts[i].data, 1, parts[i].size, f) == parts[i].size;
    }
    ok = fclose(f) == 0 && ok;
#ifdef _WIN32
    ok = ok && MoveFileExA(tmp, path, MOVEFILE_REPLACE_EXISTING);
#else
    ok = ok && rename(tmp, path) == 0;
#endif
    if (!ok) {
        nc_log("xbased-chihiro", "can't replace %s", path);
        remove(tmp);
        return false;
    }
    return true;
}

/* Like chihiro_card_issue in Tovarichtch/xemu, reserve a new card's name
 * exclusively. Keep xbased's existing filenames and 69-byte stripes. */
static bool save_card(char *path, size_t cap, const uint8_t *data)
{
    const FilePart part = {data, NC_CARD_BYTES};
    if (path[0]) {
        return file_replace(path, &part, 1);
    }
    if (!g_cards[0]) {
        return false;
    }
    nc_mkdir(g_cards);
    for (int slot = 1; slot <= CARD_SLOT_MAX; slot++) {
        char candidate[NC_CARD_PATH_CAP];
        const int len = snprintf(candidate, sizeof(candidate), "%s/card%03d.bin", g_cards, slot);
        if (len < 0 || (size_t)len >= sizeof(candidate) || (size_t)len >= cap) {
            return false;
        }
        const int fd = open(candidate, O_WRONLY | O_CREAT | O_EXCL, 0644);
        if (fd < 0) {
            if (errno == EEXIST) {
                continue;
            }
            nc_log("xbased-chihiro", "card: can't create %s: %s", candidate, strerror(errno));
            return false;
        }
        if (close(fd) != 0 || !file_replace(candidate, &part, 1)) {
            remove(candidate);
            return false;
        }
        memcpy(path, candidate, (size_t)len + 1);
        nc_log("xbased-chihiro", "card: a new card, %s", path);
        return true;
    }
    return false;
}

static void card_note(const char *what)
{
    nc_log("xbased-chihiro", "card: %s", what);
}

static void card_key(void)
{
    NcCardReader *r = &nc_chihiro_card_reader;
    if (nc_card_reader_take_back(r)) {
        card_note("taken back");
        return;
    }
    if (!nc_card_reader_can_offer(r)) {
        card_note("insert ignored while reader holds a card");
        return;
    }
    char name[NC_CHIHIRO_CARD_NAME_CAP];
    nc_chihiro_card_selected(name);
    if (!name[0]) {
        find_card(name, "", 0);
    }
    char path[NC_CARD_PATH_CAP] = "";
    struct stat st;
    if (name[0] && !card_file(name, path, &st)) {
        nc_log("xbased-chihiro", "card: unavailable %s", name);
        return;
    }
    if (nc_card_reader_offer(r, path)) {
        nc_log("xbased-chihiro", "card: offered %s", path[0] ? path : "a blank card");
    } else {
        nc_log("xbased-chihiro", "card: rejected %s", path);
    }
}

/* ------------------------------------------------------------ the chips */

static void jvs_outputs(void *opaque, const uint8_t outputs[4])
{
    nc_v257_motor(&nc_chihiro_v257, (outputs[0] & 0x80) != 0);
}

static ChihiroUsbBoard g_qc, g_sc;
static atomic_bool g_active;
static unsigned g_frames;
static bool g_card_pressed;
static void poll_input(void);

static void save_load(void)
{
    if (!g_save[0]) {
        return;
    }
    FILE *f = fopen(g_save, "rb");
    if (!f) {
        return;
    }
    uint8_t settings[CHIHIRO_SETTINGS_SIZE], sram[CHIHIRO_SRAM_SIZE];
    const bool ok = fread(settings, 1, sizeof(settings), f) == sizeof(settings) &&
                    fread(sram, 1, sizeof(sram), f) == sizeof(sram);
    fclose(f);
    if (ok) {
        memcpy(g_qc.settings, settings, sizeof(settings));
        memcpy(g_qc.sram, sram, sizeof(sram));
        nc_log("xbased-chihiro", "board memory from %s", g_save);
    }
}

/* Link play: the cabinet's PCB ID (0-based) in Maximum Tune's backup record
 * ("SBKD0000" for 2, "SBHQ0000" for 1: +0xA the ID, +0 the u32 byte sum of
 * the rest), as its test menu's GAME ASSIGNMENTS sets it. The game picks
 * its network address from it; two cabinets with one ID can't see each
 * other. Layout from Tovarichtch/xemu chihiro-usb.c. */
static void link_pcb_id(unsigned cabinet)
{
    static const char *const tags[] = {"SBKD0000", "SBHQ0000"};
    uint8_t *mem = g_qc.sram;
    for (int t = 0; t < 2; t++) {
        for (size_t pos = 0x8000; pos + 12 <= sizeof(g_qc.sram); pos++) {
            if (memcmp(mem + pos, tags[t], 8)) {
                continue;
            }
            const size_t size = (size_t)(mem[pos + 8] | mem[pos + 9] << 8) * 4;
            uint8_t *data = mem + pos + 12;
            if (size < 0x10 || pos + 12 + size > sizeof(g_qc.sram)) {
                return;
            }
            if (data[0xA] != cabinet - 1) {
                data[0xA] = (uint8_t)(cabinet - 1);
                uint32_t sum = 0;
                for (size_t i = 4; i < size; i++) {
                    sum += data[i];
                }
                nc_wr32(data, sum);
                g_qc.backup_dirty = true;
                nc_log("xbased-chihiro", "link: PCB ID %u in the game's backup", cabinet);
            }
            return;
        }
    }
}

static unsigned g_link_cabinet;

/* A record the game writes for the first time (no backup yet) gets the
 * cabinet's PCB ID too; the game reads it from its next start. */
static void backup_written(void)
{
    link_pcb_id(g_link_cabinet);
}

void nc_chihiro_board_flush(void)
{
    if (!g_active || !g_save[0] || !g_qc.backup_dirty) {
        return;
    }
    const FilePart parts[] = {{g_qc.settings, sizeof(g_qc.settings)}, {g_qc.sram, sizeof(g_qc.sram)}};
    if (file_replace(g_save, parts, 2)) {
        g_qc.backup_dirty = false;
    }
}

void nc_chihiro_board_init(const char *dir, const char *save, const NcChihiroCoinSettings *coins,
                           unsigned cabinet)
{
    snprintf(g_cards, sizeof(g_cards), "%s/cards", dir);
    nc_mkdir(dir);
    snprintf(g_save, sizeof(g_save), "%s", save ? save : "");

    nc_chihiro_card_reader.save_card = save_card;
    nc_chihiro_card_reader.note = card_note;
    nc_card_reader_init(&nc_chihiro_card_reader);
    nc_v257_init(&nc_chihiro_v257);

    chihiro_usb_init(&g_qc, true);
    chihiro_usb_init(&g_sc, false);
    g_qc.jvs.output = jvs_outputs;
    /* the cabinet's region (1 Japan, 2 USA, 3 Export) and its complement */
    const char *region = getenv("NC_CHIHIRO_REGION");
    const int r = region ? atoi(region) : 1;
    g_qc.eeprom[0x1f00] = (uint8_t)(r >= 1 && r <= 3 ? r : 1);
    g_qc.eeprom[0x1f01] = (uint8_t)~g_qc.eeprom[0x1f00];
    /* the baseboard's serial (the game's MAIN BD ID), where Sega's QC EEPROM has it */
    const char *main_id = getenv("NC_CHIHIRO_MAIN_ID");
    if (main_id && strlen(main_id) == 16) {
        memcpy(g_qc.eeprom + 0x1f10, main_id, 16);
    }
    save_load();
    chihiro_usb_apply_coins(&g_qc, coins);
    if (cabinet) {
        g_link_cabinet = cabinet;
        link_pcb_id(cabinet);
        g_qc.backup_written = backup_written;
    }
    g_frames = 0;
    g_active = true;
    nc_log("xbased-chihiro", "baseboard: QC and SC USB protocol emulation");
}

/* SEGABOOT's form for both boards' serials, "%%%@-##@########": % a digit
 * or a letter, @ a letter (I and O never), # a digit. */
static bool id_form(const char *id)
{
    static const char form[] = "%%%@-##@########";
    if (!id || strlen(id) != sizeof(form) - 1) {
        return false;
    }
    for (int i = 0; form[i]; i++) {
        const char c = id[i];
        const bool digit = c >= '0' && c <= '9', letter = c >= 'A' && c <= 'Z' && c != 'I' && c != 'O';
        const bool ok = form[i] == '#'   ? digit
                        : form[i] == '%' ? digit || letter
                        : form[i] == '@' ? letter
                                         : c == form[i];
        if (!ok) {
            return false;
        }
    }
    return true;
}

void nc_chihiro_board_id(const char *id, const char console_serial[12], bool base, char out[17])
{
    if (id_form(id)) {
        memcpy(out, id, 17);
        return;
    }
    if (id && *id) {
        nc_log("xbased-chihiro", "%s BD ID \"%s\" isn't in SEGABOOT's form: the default", base ? "MAIN" : "MEDIA", id);
    }
    /* A, two hex digits and a letter from A to F of the console's serial's
     * hash, -01D, eight of its digits (the main board's from the start, the
     * media board's from the end) */
    uint32_t h = base ? 0x811C9DC5u : 0x01000193u;
    for (int i = 0; i < 12; i++) {
        h = (h ^ (uint8_t)console_serial[i]) * 16777619u;
    }
    char digits[9];
    for (int i = 0; i < 8; i++) {
        const char c = console_serial[base ? i : 4 + i];
        digits[i] = c >= '0' && c <= '9' ? c : '0';
    }
    digits[8] = 0;
    snprintf(out, 17, "A%02X%c-01D%s", (unsigned)(h & 0xFF), 'A' + (int)((h >> 8) % 6), digits);
}

bool nc_chihiro_board_active(void)
{
    return g_active;
}

NcUsbDev *nc_chihiro_board_qc(void)
{
    return g_active ? &g_qc.usb : NULL;
}

NcUsbDev *nc_chihiro_board_sc(void)
{
    return g_active ? &g_sc.usb : NULL;
}

bool nc_chihiro_board_force(NcChihiroForce *force)
{
    return g_active && nc_v257_force(&nc_chihiro_v257, force);
}

void nc_chihiro_board_frame(void)
{
    if (!g_active) {
        return;
    }
    poll_input();
    if (g_card_pressed) {
        g_card_pressed = false;
        card_key();
    }
    if (++g_frames % 16 == 0) {
        nc_v257_tick(&nc_chihiro_v257);
    }
    if (g_frames % 5000 == 0) {
        nc_chihiro_board_flush();
    }
}

/* ------------------------------------------------------------ controls */

/* H shifter switch bits per gear (up 0x80, down 0x40, left 0x20, right 0x10). */
static const uint8_t k_gate[7] = {0x00, 0xA0, 0x60, 0x80, 0x40, 0x90, 0x50};

static pthread_mutex_t g_input_lock = PTHREAD_MUTEX_INITIALIZER;
static NcChihiroInput g_pad_in = {.steering = 0x8000}, g_wheel_in;
static bool g_wheel;

static void apply_input(const NcChihiroInput *in);

/* Merges pad and wheel: the wheel's axes when present, either's buttons. */
static void poll_input(void)
{
    pthread_mutex_lock(&g_input_lock);
    NcChihiroInput in = g_pad_in;
    const NcChihiroInput *w = &g_wheel_in;
    if (g_wheel) {
        in.steering = w->steering;
        in.gas = w->gas;
        in.brake = w->brake;
    }
    if (w->h_shifter) {
        in.h_shifter = true;
        in.gear = w->gear;
    }
    in.test |= w->test;
    in.service |= w->service;
    in.coin |= w->coin;
    in.start |= w->start;
    in.view |= w->view;
    in.perspective |= w->perspective;
    in.shift_up |= w->shift_up;
    in.shift_down |= w->shift_down;
    in.card |= w->card;
    pthread_mutex_unlock(&g_input_lock);
    apply_input(&in);
}

void nc_chihiro_board_input(const NcChihiroInput *in)
{
    if (!g_active) {
        return;
    }
    pthread_mutex_lock(&g_input_lock);
    g_pad_in = *in;
    pthread_mutex_unlock(&g_input_lock);
}

void nc_chihiro_board_wheel(bool axes, const NcChihiroInput *in)
{
    if (!g_active) {
        return;
    }
    pthread_mutex_lock(&g_input_lock);
    g_wheel = axes;
    g_wheel_in = *in;
    pthread_mutex_unlock(&g_input_lock);
}

static int64_t g_test_at_ns;    /* host time TEST was last held */

bool nc_chihiro_board_test_pressed(void)
{
    const int64_t at = __atomic_load_n(&g_test_at_ns, __ATOMIC_ACQUIRE);
    return g_active && at && host_ns() - at < 3000000000LL;
}

static uint16_t wmmt_pedal(uint16_t travel)
{
    /* The game reaches full pedal at 320 of the JVS channel's 1023 steps. */
    const uint32_t saturate = UINT16_MAX * 95u / 100u;
    return MIN(travel, saturate) * (320u << 6) / saturate;
}

static void apply_input(const NcChihiroInput *in)
{
    static int gear;
    if (in->test) {
        __atomic_store_n(&g_test_at_ns, host_ns(), __ATOMIC_RELEASE);
    }
    static bool up_before, down_before, coin_before, card_before;
    if (!in->h_shifter && in->shift_up && !up_before && gear < 6) {
        gear++;
    }
    if (!in->h_shifter && in->shift_down && !down_before && gear > 0) {
        gear--;
    }
    up_before = in->shift_up;
    down_before = in->shift_down;

    NcJvs *j = &g_qc.jvs;
    j->analog[0] = in->steering;
    j->analog[1] = wmmt_pedal(in->gas);
    j->analog[2] = wmmt_pedal(in->brake);
    nc_v257_wheel(&nc_chihiro_v257, in->steering >> 6);
    j->system_switches = in->test ? 0x80 : 0x00;
    j->player_switches[0][0] = (in->start ? 0x80 : 0) | (in->service ? 0x40 : 0);
    const uint8_t gate = k_gate[in->h_shifter ? (in->gear <= 6 ? in->gear : 0) : gear];
    j->player_switches[0][1] = gate | (in->view ? 0x02 : 0) | (in->perspective ? 0x01 : 0);
    if (in->coin && !coin_before) {
        j->coin_count[0]++;
    }
    coin_before = in->coin;
    if (in->card && !card_before) {
        g_card_pressed = true;
    }
    card_before = in->card;
}
