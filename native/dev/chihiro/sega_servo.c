/*
 * Chihiro drive board for OutRun 2: Sega's servo motor drive board
 * ("SERVO MOTOR DRIVE BD MIDI", 837-13551-92 in the Deluxe cabinet's parts
 * list), on the SC's second serial port at MIDI speed.
 *
 * Game -> board, 4 bytes:
 *   command | 0x80, p1, p2, (command ^ p1 ^ p2) & 0x7F
 * Board -> game, 1 byte for each frame: the board's status in bits 0-2,
 * the same 3 bits again in bits 4-6 (bit 3 set = a bad byte). 0 means
 * ready; anything else in play is a fault ("DRIVE BOARD FAULT"). The game
 * counts that every frame gets exactly one answer, and polls with 7D
 * whenever it has nothing else to send.
 *
 * Start-up (outrun2.xbe, state machine at 0x0001d160):
 *   7F reset        board answers 1 (initialising)
 *   01 30 7F mode   must answer 1, or the game resets again
 *   7C 00 20 start  the board finds its centre: the wheel turns to full
 *                   left and right lock and settles in the middle (the
 *                   manual's power-on check)
 *   7D poll         repeated until the board answers 0; the game gives up
 *                   after 480 frames (8 s)
 * Two failed tries and the game shows "Drive board is not connected to
 * main board." Then the game checks the centre itself (0x0001dd80): motor
 * on, a torque (84) nudged by 10 every frame until its steering value
 * reads exactly 0x80, and 70 once it does. After 600 frames (10 s) it
 * shows "Drive board : Centering error".
 *
 * In play (0x0001d3e0 works the values out every frame, 0x0001d800 sends
 * the ones that changed). Positions are the game's steering value (the top
 * 8 bits of the JVS channel, 0x80 the middle); torques are 8 bits with 0x80
 * meaning none, sent as p1 = bit 7, p2 = the rest.
 *   80 p1 p2      motor on (01 01) or off (00 00)
 *   83 power 04   overall strength, the test menu's MOTOR POWER: 32, 40,
 *                 50, 60 for 60, 80, 90, 100 %
 *   84 torque     a push; below 0x80 to the right, above to the left
 *   87 position   the spring's centre, sent as 0xFF - position. Driving:
 *                 the CENTRE OF STEER setting. Stopped: it trails the
 *                 player's wheel by 0x1F, so the wheel feels dragged.
 *   8B level rate the centring strength, level * 8 from the car's speed
 *                 (4-15 * 8); rate 2 * 2 while it rises, 0x1F * 2 while
 *                 it falls
 *   88 00 damper  8 stopped, 4 moving
 *   86 2F drag    2 moving, 1 at the slowest level
 *   9D index pkg  then 9E torque: 16 "packages" of 16 torques each,
 *                 uploaded once after centring (table at 0x001c8c28); the
 *                 first byte of every package is 0x10, taken as a header
 *   FB 00 pkg     play a package: bump, wall, car, kerb, grass ... (event
 *                 table at 0x001c8d28); bit 4 mirrors it (the side hit)
 * What the real board makes of these, and how strongly, isn't documented;
 * the mapping onto the host wheel's spring, damper and push is ours.
 */
#include "drive.h"
#include "board.h"
#include "trace.h"

#include "native/core/nc.h"

#include <string.h>

enum {
    MOTOR = 0x00,
    POWER = 0x03,
    TORQUE = 0x04,
    DRAG = 0x06,
    SPRING_AT = 0x07,
    DAMPER = 0x08,
    CENTRING = 0x0B,
    PACKAGE_AT = 0x1D,
    PACKAGE_PUT = 0x1E,
    CENTRED = 0x70,
    PLAY = 0x7B,
    START = 0x7C,
    RESET = 0x7F,
};

enum { READY = 0, INITIALISING = 1 };

#define TICKS_PER_S (1000 / 16)
/* The centre search: a third of it at each lock, then the middle; about
 * 3 s in all, well inside the game's 8 s. */
#define ROLL_TICKS (TICKS_PER_S * 3)
/* The longest the board holds the wheel: the game's 8 s start-up and 10 s
 * centring, so it isn't kept if the game never says it's centred. */
#define HOLD_TICKS (TICKS_PER_S * 18)
#define MIDDLE 0x80     /* the game's steering value for the centre */

static struct {
    uint8_t frame[4];
    int frame_len;
    uint8_t answers[16];        /* status bytes waiting for the game */
    int head, tail;
    int status;

    /* start-up */
    int rolling;                /* ticks left in the centre search */
    int holding;                /* ticks left holding the wheel, START to CENTRED */

    /* play */
    bool motor;
    int8_t push;                /* 84, positive turns the wheel right */
    uint8_t power;              /* 83: 0x32-0x60 */
    uint8_t spring_at;          /* 87: the spring's centre, 0-0xFF */
    uint8_t centring_level;     /* 8B: where the centring strength is going */
    uint8_t centring_rate;      /* 8B: how far it moves a tick */
    int centring_now;           /* the centring strength now */
    uint8_t damper, drag;       /* 88, 86 */
    uint8_t package[16][16];    /* 9D/9E: torques, 0x80 none */
    uint8_t put_index, put_package;
    int playing;                /* the package FB plays, -1 none */
    int play_step;
    bool play_mirror;
} g;

static int clamp(int v, int lo, int hi)
{
    return v < lo ? lo : v > hi ? hi : v;
}

/* A torque sent as p1 = bit 7, p2 = the rest, as a push (positive right). */
static int push_of(uint8_t p1, uint8_t p2)
{
    return clamp(0x80 - ((p1 & 1) << 7 | p2), -0x7F, 0x7F);
}

/* A steering value (0-0xFF) as the host wheel's target (0-0x3FF): the
 * middle of the values the game reads as it. */
static uint16_t target_of(unsigned position)
{
    return (uint16_t)((position << 8 | 0x80) >> 6);
}

static void answer(void)
{
    const int next = (g.tail + 1) % (int)sizeof(g.answers);
    if (next != g.head) {
        g.answers[g.tail] = (uint8_t)(g.status | g.status << 4);
        g.tail = next;
    }
}

static void command(const uint8_t f[4])
{
    static unsigned budget = 200;
    if (chihiro_trace(&budget)) {
        char h[CHIHIRO_HEX_CAP];
        nc_log("xbased-chihiro", "drive board <-%s", chihiro_hex(f, 4, h));
    }
    switch (f[0] & 0x7F) {
    case RESET:
        g.status = INITIALISING;
        g.rolling = g.holding = 0;
        g.motor = false;
        g.push = 0;
        break;
    case START:
        g.rolling = ROLL_TICKS;
        g.holding = HOLD_TICKS;
        break;
    case CENTRED:
        g.holding = 0;
        break;
    case MOTOR:
        g.motor = f[1] || f[2];
        break;
    case POWER:
        g.power = f[1];
        break;
    case TORQUE:
        g.push = (int8_t)push_of(f[1], f[2]);
        break;
    case SPRING_AT:
        g.spring_at = (uint8_t)(0xFF - ((f[1] & 1) << 7 | f[2]));
        break;
    case CENTRING:
        g.centring_level = f[1];
        g.centring_rate = f[2] ? f[2] : 1;
        break;
    case DAMPER:
        g.damper = f[2];
        break;
    case DRAG:
        g.drag = f[2];
        break;
    case PACKAGE_AT:
        g.put_index = f[1] & 15;
        g.put_package = f[2] & 15;
        break;
    case PACKAGE_PUT:
        g.package[g.put_package][g.put_index] = (uint8_t)((f[1] & 1) << 7 | f[2]);
        break;
    case PLAY:
        g.playing = f[2] & 15;
        g.play_mirror = (f[2] & 0x10) != 0;
        g.play_step = 1;                /* byte 0 is the header */
        break;
    }
    answer();
}

static void drive_init(void)
{
    memset(&g, 0, sizeof(g));
    g.status = INITIALISING;
    g.power = 0x40;
    g.spring_at = MIDDLE;
    g.playing = -1;
    memset(g.package, 0x80, sizeof(g.package));
}

static void drive_tick(void)
{
    if (g.rolling > 0 && --g.rolling == 0) {
        g.status = READY;
    }
    if (g.holding > 0) {
        g.holding--;
    }
    /* The centring strength moves toward its level at the game's rate. */
    const int level = g.centring_level;
    if (g.centring_now < level) {
        g.centring_now = clamp(g.centring_now + g.centring_rate, 0, level);
    } else if (g.centring_now > level) {
        g.centring_now = clamp(g.centring_now - g.centring_rate, level, 0xFF);
    }
    /* A package plays one torque a tick. */
    if (g.playing >= 0 && ++g.play_step >= 16) {
        g.playing = -1;
    }
}

static void drive_rx(uint8_t byte)
{
    if (byte & 0x80) {
        g.frame_len = 0;
    } else if (!g.frame_len) {
        return;                         /* no command byte yet */
    }
    g.frame[g.frame_len++] = byte;
    if (g.frame_len < 4) {
        return;
    }
    g.frame_len = 0;
    if (((g.frame[0] ^ g.frame[1] ^ g.frame[2]) & 0x7F) != g.frame[3]) {
        return;                         /* a damaged frame isn't answered */
    }
    command(g.frame);
}

static bool drive_tx(uint8_t *byte)
{
    if (g.head == g.tail) {
        return false;
    }
    *byte = g.answers[g.head];
    g.head = (g.head + 1) % (int)sizeof(g.answers);
    return true;
}

/*
 * The centre search turns the wheel one way, the other way and back. The
 * real board goes to full lock; here a quarter turn each way, so a strong
 * wheel isn't thrown against its stops. Until the game says the wheel is
 * centred, the board then holds it firmly in the middle, the game's own
 * torque on top.
 *
 * In play: the spring at 87's position with 8B's strength, the damper from
 * 88 and 86, and the push of 84 plus the package playing. The game's
 * strengths are for the cabinet's servo at its MOTOR POWER; 0x60 (100 %)
 * is taken as the host wheel's full strength.
 */
static bool drive_force(NcChihiroForce *force)
{
    *force = (NcChihiroForce){.target = target_of(MIDDLE), .damping = 0x20};
    if (g.rolling > 0) {
        const int third = 2 - (g.rolling - 1) / (ROLL_TICKS / 3);   /* 0 left, 1 right, 2 middle */
        force->target = third == 0 ? 0x100 : third == 1 ? 0x300 : target_of(MIDDLE);
        force->spring = third == 2 ? 0x7F : 0x30;
        return true;
    }
    if (g.holding > 0) {
        force->spring = 0x7F;
        force->push = g.motor ? g.push : 0;
        return true;
    }
    if (!g.motor) {
        return false;
    }
    const int power = g.power ? g.power : 0x40;
    force->target = target_of(g.spring_at);
    force->spring = (uint8_t)clamp(g.centring_now * power / 0x60, 0, 0x7F);
    force->damping = (uint8_t)clamp(g.damper * 8 + g.drag * 4, 0, 0x7F);
    int push = g.push;
    if (g.playing >= 0) {
        const uint8_t torque = g.package[g.playing][g.play_step];
        const int bump = push_of(torque >> 7, torque & 0x7F) * power / 0x60;
        push += g.play_mirror ? -bump : bump;
    }
    force->push = (int8_t)clamp(push, -0x7F, 0x7F);
    return true;
}

/*
 * From START until the game reports the wheel centred, the board has the
 * wheel in its grip (the manual: hands off the wheel during the power-on
 * check), so the game reads the position the board holds it at.
 *
 * Why: the game's check wants its steering value to read exactly 0x80, a
 * band 0.4 % of the wheel's travel wide. A home wheel held by a spring stops
 * short of it because of its friction (1.5 % off on a Gudsen R3), its force
 * feedback may be off (the window in the background) or it has none, and
 * the game stopped with "Drive board : Centering error".
 */
static bool drive_holding(uint16_t *steering)
{
    if (g.holding <= 0) {
        return false;
    }
    *steering = MIDDLE << 8 | 0x80;     /* the middle of the band */
    return true;
}

const NcChihiroDrive nc_chihiro_drive_sega_servo = {
    .name = "Sega servo motor drive board (OutRun 2)",
    .init = drive_init,
    .tick = drive_tick,
    .rx = drive_rx,
    .tx = drive_tx,
    .force = drive_force,
    .holding = drive_holding,
};
