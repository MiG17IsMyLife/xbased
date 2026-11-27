/*
 * Chihiro card files (cards.h): picks the card file to insert, hands it to
 * the card reader (crp1231.c), and saves what the game writes on it.
 */
#include "native/dev/chihiro/cards.h"

#include "board_int.h"
#include "file_util.h"

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

static char g_cards[512];           /* the card folder, <cabinet folder>/cards */
static pthread_mutex_t g_card_lock = PTHREAD_MUTEX_INITIALIZER;
static char g_card_name[NC_CHIHIRO_CARD_NAME_CAP];  /* the chosen card, "" the newest */

/* ---------------------------------------------------------- card names */

/* A plain file name in the card folder: no path separators, not "." or
 * "..", no trailing space, and not a temporary file being written. */
static bool valid_card_name(const char *name)
{
    const size_t n = strlen(name);
    if (n > 4 && !strcasecmp(name + n - 4, ".tmp")) {
        return false;               /* chihiro_file_replace's temporary file */
    }
    return *name && strlen(name) < NC_CHIHIRO_CARD_NAME_CAP &&
           strcmp(name, ".") && strcmp(name, "..") && !strpbrk(name, "/\\:\r\n") &&
           name[strlen(name) - 1] != ' ' && name[strlen(name) - 1] != '\t';
}

/* True if name is a card file: a regular file that is empty (a blank card)
 * or exactly NC_CARD_BYTES long. Fills in its path and stat. */
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

/*
 * Searches the card folder.
 *   direction > 0  the first card whose name sorts after current
 *   direction < 0  the last card whose name sorts before current
 *                  (with current "": the last card of all)
 *   direction 0    the most recently modified card
 * result is "" when there is none.
 */
static void find_card(char result[NC_CHIHIRO_CARD_NAME_CAP], const char *current, int direction)
{
    result[0] = 0;
    if (!g_cards[0]) {
        return;
    }
    /* On Windows the folder is read with the wide-character API so that
     * UTF-8 names work. */
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
        /* Skip names on the wrong side of current, or no closer to it
         * than the best found so far. */
        if ((direction > 0 && (relative <= 0 || (*result && order >= 0))) ||
            (direction < 0 && ((*current && relative >= 0) || (*result && order <= 0)))) {
            continue;
        }
        char path[NC_CARD_PATH_CAP];
        struct stat st;
        /* For direction 0, keep the newest (ties: the later name). */
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

/* ---------------------------------------------------------- the reader */

/*
 * The card reader's save callback: the game wrote data onto the card.
 * path is the card's file, or "" for a blank card that has no file yet.
 * A new card gets the first free name card001.bin .. card999.bin; the
 * name is claimed with O_EXCL so two running cabinets can't pick the same
 * one (as chihiro_card_issue in Tovarichtch/xemu does).
 */
static bool save_card(char *path, size_t cap, const uint8_t *data)
{
    const ChihiroFilePart part = {data, NC_CARD_BYTES};
    if (path[0]) {
        return chihiro_file_replace(path, &part, 1);
    }
    if (!g_cards[0]) {
        return false;
    }
    chihiro_mkdir(g_cards);
    for (int slot = 1; slot <= CARD_SLOT_MAX; slot++) {
        char candidate[NC_CARD_PATH_CAP];
        const int len = snprintf(candidate, sizeof(candidate), "%s/card%03d.bin", g_cards, slot);
        if (len < 0 || (size_t)len >= sizeof(candidate) || (size_t)len >= cap) {
            return false;
        }
        const int fd = open(candidate, O_WRONLY | O_CREAT | O_EXCL, 0644);
        if (fd < 0) {
            if (errno == EEXIST) {
                continue;           /* taken, try the next number */
            }
            nc_log("xbased-chihiro", "card: can't create %s: %s", candidate, strerror(errno));
            return false;
        }
        if (close(fd) != 0 || !chihiro_file_replace(candidate, &part, 1)) {
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

void chihiro_cards_init(const char *dir)
{
    snprintf(g_cards, sizeof(g_cards), "%s/cards", dir);
    chihiro_mkdir(dir);
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
    if (nc_card_reader_offer(r, path)) {
        nc_log("xbased-chihiro", "card: offered %s", path[0] ? path : "a blank card");
    } else {
        nc_log("xbased-chihiro", "card: rejected %s", path);
    }
}
