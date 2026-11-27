/*
 * Chihiro card files (cards.h): picks the card file to insert, hands it to
 * the card reader (crp1231.c), and saves what the game writes on it.
 *
 * Cards come from three folders (storage.h):
 *   own       this game's cards
 *   unsorted  cards from before cards were split by game; one moves into
 *             the own folder when the game first writes it
 *   transfer  the previous game's cards, named "<label>:<file>" (as
 *             "WMMT1:card001.bin"); inserting one inserts a copy kept in
 *             the own folder, "<file> (<label>).bin"
 * New cards always go into the own folder.
 */
#include "native/dev/chihiro/cards.h"

#include "board_int.h"
#include "file_util.h"

#include <ctype.h>
#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <time.h>
#include <unistd.h>

#ifdef _WIN32
#include <windows.h>
#endif

enum { CARD_SLOT_MAX = 999 };       /* new cards: card001.bin .. card999.bin */

/* The card reader on the SC's first serial port. */
NcCardReader nc_chihiro_card_reader;

static char g_own[NC_CHIHIRO_PATH_CAP];       /* this game's card folder */
static char g_unsorted[NC_CHIHIRO_PATH_CAP];  /* cards from before the split */
static char g_transfer[NC_CHIHIRO_PATH_CAP];  /* the previous game's, "" none */
static char g_label[8];                       /* its name prefix, "WMMT1" */

static pthread_mutex_t g_card_lock = PTHREAD_MUTEX_INITIALIZER;
static char g_card_name[NC_CHIHIRO_CARD_NAME_CAP];  /* the chosen card, "" the newest */

/* ---------------------------------------------------------- card names */

/* A plain file name in a card folder: no path separators, not "." or
 * "..", no trailing space, and not a temporary file being written. */
static bool valid_file_name(const char *name)
{
    const size_t n = strlen(name);
    if (n > 4 && !strcasecmp(name + n - 4, ".tmp")) {
        return false;               /* chihiro_file_replace's temporary file */
    }
    return *name && n < NC_CHIHIRO_CARD_NAME_CAP &&
           strcmp(name, ".") && strcmp(name, "..") && !strpbrk(name, "/\\:\r\n") &&
           name[n - 1] != ' ' && name[n - 1] != '\t';
}

/* If name is a transfer card ("WMMT1:card001.bin"), returns its file name
 * part and sets *prefix_len to the label's length; otherwise NULL. */
static const char *transfer_file(const char *name, size_t *prefix_len)
{
    const char *colon = strchr(name, ':');
    if (!colon) {
        return NULL;
    }
    *prefix_len = (size_t)(colon - name);
    return colon + 1;
}

/* A card name as the player picks it: a plain file name, or a label of
 * letters and digits, ':' and a plain file name. */
static bool valid_card_name(const char *name)
{
    size_t prefix;
    const char *file = transfer_file(name, &prefix);
    if (!file) {
        return valid_file_name(name);
    }
    if (!prefix || prefix >= sizeof(g_label) || strlen(name) >= NC_CHIHIRO_CARD_NAME_CAP) {
        return false;
    }
    for (size_t i = 0; i < prefix; i++) {
        if (!isalnum((unsigned char)name[i])) {
            return false;
        }
    }
    return valid_file_name(file);
}

/* True if folder/name is a card file: a regular file that is empty (a
 * blank card) or exactly NC_CARD_BYTES long. Fills in its path and stat. */
static bool card_in(const char *folder, const char *name, char path[NC_CARD_PATH_CAP], struct stat *st)
{
    if (!folder[0] || !valid_file_name(name)) {
        return false;
    }
    const int n = snprintf(path, NC_CARD_PATH_CAP, "%s/%s", folder, name);
    return n >= 0 && n < NC_CARD_PATH_CAP && stat(path, st) == 0 && S_ISREG(st->st_mode) &&
           (st->st_size == 0 || st->st_size == NC_CARD_BYTES);
}

/* Finds the file for a card name: a transfer card in the transfer folder,
 * any other first in the own folder, then in the unsorted one. */
static bool card_file(const char *name, char path[NC_CARD_PATH_CAP], struct stat *st)
{
    size_t prefix;
    const char *file = transfer_file(name, &prefix);
    if (file) {
        return g_transfer[0] && prefix == strlen(g_label) && !strncmp(name, g_label, prefix) &&
               card_in(g_transfer, file, path, st);
    }
    return card_in(g_own, name, path, st) || card_in(g_unsorted, name, path, st);
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

/* ---------------------------------------------------------- searching */

/* What find_card is looking for, and the best card found so far. */
typedef struct CardSearch {
    const char *current;
    int direction;
    char *result;
    time_t latest;
} CardSearch;

/* Considers one card name for the search (see find_card). */
static void consider(CardSearch *s, const char *name)
{
    const int relative = strcmp(name, s->current);
    const int order = strcmp(name, s->result);
    /* Skip names on the wrong side of current, or no closer to it than
     * the best found so far. */
    if ((s->direction > 0 && (relative <= 0 || (*s->result && order >= 0))) ||
        (s->direction < 0 && ((*s->current && relative >= 0) || (*s->result && order <= 0)))) {
        return;
    }
    char path[NC_CARD_PATH_CAP];
    struct stat st;
    /* For direction 0, keep the newest (ties: the later name). */
    if (card_file(name, path, &st) &&
        (s->direction || !*s->result || st.st_mtime > s->latest || (st.st_mtime == s->latest && order > 0))) {
        strcpy(s->result, name);
        s->latest = st.st_mtime;
    }
}

/* Considers every file in folder, each named prefix + its file name. */
static void scan_folder(CardSearch *s, const char *folder, const char *prefix)
{
    if (!folder[0]) {
        return;
    }
    const size_t skip = strlen(prefix);
    char name[NC_CHIHIRO_CARD_NAME_CAP];
    if (skip >= sizeof(name)) {
        return;
    }
    memcpy(name, prefix, skip);
    /* On Windows the folder is read with the wide-character API so that
     * UTF-8 names work. */
#ifdef _WIN32
    wchar_t directory[NC_CHIHIRO_PATH_CAP];
    if (!MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, folder, -1,
                             directory, sizeof(directory) / sizeof(*directory))) {
        return;
    }
    _WDIR *dir = _wopendir(directory);
    if (!dir) {
        return;
    }
    for (struct _wdirent *entry; (entry = _wreaddir(dir));) {
        if (WideCharToMultiByte(CP_UTF8, 0, entry->d_name, -1, name + skip, (int)(sizeof(name) - skip), NULL, NULL)) {
            consider(s, name);
        }
    }
    _wclosedir(dir);
#else
    DIR *dir = opendir(folder);
    if (!dir) {
        return;
    }
    for (struct dirent *entry; (entry = readdir(dir));) {
        if (snprintf(name + skip, sizeof(name) - skip, "%s", entry->d_name) < (int)(sizeof(name) - skip)) {
            consider(s, name);
        }
    }
    closedir(dir);
#endif
}

/*
 * Searches the card folders, all names sorted together.
 *   direction > 0  the first card whose name sorts after current
 *   direction < 0  the last card whose name sorts before current
 *                  (with current "": the last card of all)
 *   direction 0    the most recently modified card of the own and
 *                  unsorted folders (never another game's card)
 * result is "" when there is none.
 */
static void find_card(char result[NC_CHIHIRO_CARD_NAME_CAP], const char *current, int direction)
{
    result[0] = 0;
    CardSearch s = {current, direction, result, 0};
    scan_folder(&s, g_own, "");
    scan_folder(&s, g_unsorted, "");
    if (direction && g_label[0]) {
        char prefix[sizeof(g_label) + 1];
        snprintf(prefix, sizeof(prefix), "%s:", g_label);
        scan_folder(&s, g_transfer, prefix);
    }
}

void nc_chihiro_card_step(char name[NC_CHIHIRO_CARD_NAME_CAP], int direction)
{
    char next[NC_CHIHIRO_CARD_NAME_CAP];
    find_card(next, name, direction);
    strcpy(name, next);
}

/* ---------------------------------------------------------- saving */

static bool exists(const char *path)
{
    struct stat st;
    return stat(path, &st) == 0;
}

/* If path is in the unsorted folder, the same name in the own folder. */
static bool unsorted_to_own(const char *path, char out[NC_CARD_PATH_CAP])
{
    const size_t n = strlen(g_unsorted);
    if (!g_unsorted[0] || strncmp(path, g_unsorted, n) || path[n] != '/') {
        return false;
    }
    const int len = snprintf(out, NC_CARD_PATH_CAP, "%s/%s", g_own, path + n + 1);
    return len > 0 && len < NC_CARD_PATH_CAP;
}

/* A new card: claims the first free name card001.bin .. card999.bin in the
 * own folder (also free in the unsorted folder, so names stay unique). The
 * name is claimed with O_EXCL so two running cabinets can't pick the same
 * one (as chihiro_card_issue in Tovarichtch/xemu does). */
static bool new_card(char *path, size_t cap, const ChihiroFilePart *part)
{
    for (int slot = 1; slot <= CARD_SLOT_MAX; slot++) {
        char candidate[NC_CARD_PATH_CAP], old[NC_CARD_PATH_CAP];
        const int len = snprintf(candidate, sizeof(candidate), "%s/card%03d.bin", g_own, slot);
        snprintf(old, sizeof(old), "%s/card%03d.bin", g_unsorted, slot);
        if (len < 0 || (size_t)len >= sizeof(candidate) || (size_t)len >= cap) {
            return false;
        }
        if (g_unsorted[0] && exists(old)) {
            continue;
        }
        const int fd = open(candidate, O_WRONLY | O_CREAT | O_EXCL, 0644);
        if (fd < 0) {
            if (errno == EEXIST) {
                continue;           /* taken, try the next number */
            }
            nc_log("xbased-chihiro", "card: can't create %s: %s", candidate, strerror(errno));
            return false;
        }
        if (close(fd) != 0 || !chihiro_file_replace(candidate, part, 1)) {
            remove(candidate);
            return false;
        }
        memcpy(path, candidate, (size_t)len + 1);
        nc_log("xbased-chihiro", "card: a new card, %s", path);
        return true;
    }
    return false;
}

/*
 * The card reader's save callback: the game wrote data onto the card.
 * path is the card's file, or "" for a blank card that has no file yet.
 * An unsorted card is written into the own folder instead (and the old
 * file removed), which is how it becomes this game's card.
 */
static bool save_card(char *path, size_t cap, const uint8_t *data)
{
    const ChihiroFilePart part = {data, NC_CARD_BYTES};
    if (!g_own[0]) {
        return false;
    }
    chihiro_mkdir(g_own);
    if (!path[0]) {
        return new_card(path, cap, &part);
    }
    char own[NC_CARD_PATH_CAP];
    if (unsorted_to_own(path, own) && strlen(own) < cap) {
        if (exists(own)) {
            nc_log("xbased-chihiro", "card: %s stays unsorted (%s is taken)", path, own);
        } else if (chihiro_file_replace(own, &part, 1)) {
            remove(path);
            nc_log("xbased-chihiro", "card: %s moved to %s", path, own);
            memcpy(path, own, strlen(own) + 1);
            return true;
        }
    }
    return chihiro_file_replace(path, &part, 1);
}

/* For a transfer card: the copy in the own folder that is inserted in its
 * place, made the first time. False if it can't be made. */
static bool transfer_copy(const char *name, const char *source, char path[NC_CARD_PATH_CAP])
{
    size_t prefix;
    const char *file = transfer_file(name, &prefix);
    size_t base = strlen(file);
    if (base > 4 && !strcasecmp(file + base - 4, ".bin")) {
        base -= 4;
    }
    const int n = snprintf(path, NC_CARD_PATH_CAP, "%s/%.*s (%s).bin", g_own, (int)base, file, g_label);
    if (n < 0 || n >= NC_CARD_PATH_CAP) {
        return false;
    }
    if (exists(path)) {
        return true;                /* copied before: keep using that copy */
    }
    uint8_t card[NC_CARD_BYTES];
    size_t size = 0;
    FILE *f = fopen(source, "rb");
    if (!f) {
        return false;
    }
    size = fread(card, 1, sizeof(card), f);
    fclose(f);
    chihiro_mkdir(g_own);
    const ChihiroFilePart part = {card, size};
    if (!chihiro_file_replace(path, &part, 1)) {
        return false;
    }
    nc_log("xbased-chihiro", "card: %s copied to %s", source, path);
    return true;
}

static void card_note(const char *what)
{
    nc_log("xbased-chihiro", "card: %s", what);
}

/* ---------------------------------------------------------- the reader */

void chihiro_cards_init(const NcChihiroStorage *storage)
{
    snprintf(g_own, sizeof(g_own), "%s", storage->cards);
    snprintf(g_unsorted, sizeof(g_unsorted), "%s", storage->unsorted);
    snprintf(g_transfer, sizeof(g_transfer), "%s", storage->transfer);
    snprintf(g_label, sizeof(g_label), "%s", storage->transfer[0] ? storage->transfer_label : "");
    chihiro_mkdir(storage->folder);
    nc_chihiro_card_reader.save_card = save_card;
    nc_chihiro_card_reader.note = card_note;
    nc_card_reader_init(&nc_chihiro_card_reader);
}

void chihiro_cards_button(void)
{
    NcCardReader *r = &nc_chihiro_card_reader;
    /* A card still sticking out of the slot is taken back. */
    if (nc_card_reader_take_back(r)) {
        card_note("taken back");
        return;
    }
    if (!nc_card_reader_can_offer(r)) {
        card_note("insert ignored while reader holds a card");
        return;
    }
    /* Otherwise insert the chosen card, or the newest one. With no card
     * file at all, a blank card goes in. */
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
    size_t prefix;
    if (transfer_file(name, &prefix)) {
        char source[NC_CARD_PATH_CAP];
        memcpy(source, path, sizeof(source));
        if (!transfer_copy(name, source, path)) {
            nc_log("xbased-chihiro", "card: can't copy %s", source);
            return;
        }
    }
    if (nc_card_reader_offer(r, path)) {
        nc_log("xbased-chihiro", "card: offered %s", path[0] ? path : "a blank card");
    } else {
        nc_log("xbased-chihiro", "card: rejected %s", path);
    }
}
