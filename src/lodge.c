#define _DEFAULT_SOURCE
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <time.h>
#include <sys/types.h>
#include "landscape.h"
#include <termios.h>
#include <signal.h>
#include <sys/ioctl.h>
#include <poll.h>

struct termios oldt, newt;
static int input_saved = 0;   // oldt holds the original terminal settings
static int screen_active = 0; // alternate screen is on and the cursor is hidden

#define min(x, y) ((x) < (y) ? (x) : (y))

#ifndef LODGE_VERSION
#define LODGE_VERSION "dev"
#endif

#define PIC_W LINE_LENGTH       // width of the hand-drawn picture, in cells
#define PIC_H (PIC_END + 1)     // height of the hand-drawn picture, in rows

char lines[LINES][LINE_LENGTH];
char message[1024][LINE_LENGTH];
char* landscape_file;
char* message_file;
static volatile sig_atomic_t stop_looping = 0;  // set by the signal handler, polled everywhere
static volatile sig_atomic_t hangup = 0;        // terminal is gone (SIGHUP): skip the farewell
static volatile sig_atomic_t fast_exit = 0;     // a second Ctrl+C: skip the farewell animation
static volatile sig_atomic_t interrupts = 0;    // how many Ctrl+C / SIGTERM arrived
static long g_ev_off = 0;  // shifts the time of the wind, fish, cat and snow showers, chosen once per run: every start differs
static int g_demo = 0;  // LODGE_DEMO=1: a gust in every wind window and a jump in every fish window
static volatile sig_atomic_t need_redraw = 0;   // SIGWINCH / SIGCONT: repaint everything from scratch

// --------------------------------------------------

static void print_help(const char *argv0) {
    printf("Lodge %s\n\n", LODGE_VERSION);
    printf("Usage:\n");
    printf("  %s <landscape_file> <message_file>\n", argv0);
    printf("  %s --help\n", argv0);
    printf("  %s --version\n", argv0);
}

// Only Ctrl+C (SIGINT) is allowed to reach the program: every other key is swallowed silently.
void disable_input() {
    if (tcgetattr(STDIN_FILENO, &oldt) != 0) {
        return;  // stdin is not a terminal
    }
    input_saved = 1;
    newt = oldt;

    newt.c_lflag &= ~(ICANON | ECHO | IEXTEN);  // no line editing, no echo, no Ctrl+V / Ctrl+O
    newt.c_iflag &= ~(IXON | IXOFF);            // no Ctrl+S / Ctrl+Q flow control
    newt.c_cc[VMIN] = 1;
    newt.c_cc[VTIME] = 0;
    // Keys that would otherwise suspend, quit or poke the program.
    newt.c_cc[VSUSP] = _POSIX_VDISABLE;   // Ctrl+Z
    newt.c_cc[VQUIT] = _POSIX_VDISABLE;   // Ctrl+\ .
#ifdef VDSUSP
    newt.c_cc[VDSUSP] = _POSIX_VDISABLE;  // Ctrl+Y (macOS)
#endif
#ifdef VSTATUS
    newt.c_cc[VSTATUS] = _POSIX_VDISABLE; // Ctrl+T (macOS)
#endif
    tcsetattr(STDIN_FILENO, TCSAFLUSH, &newt);
}

// Restores the terminal; also drops anything the user typed meanwhile (TCSAFLUSH) so it does not reach the shell.
void restore_input() {
    if (input_saved) {
        tcsetattr(STDIN_FILENO, TCSAFLUSH, &oldt);
        input_saved = 0;
    }
}

// Terminal modes while the scene runs: hidden cursor, no auto-wrap (so the last cell of a row can be painted safely),
// mouse reporting (1000/1006) so the wheel and clicks go to us instead of scrolling the terminal's history, and
// alternate-scroll (1007) for terminals that translate the wheel into arrow keys on the alternate screen.
#define TERM_MODES "\033[?25l\033[?7l\033[?1000h\033[?1006h\033[?1007h"
#define TERM_MODES_OFF "\033[?1007l\033[?1006l\033[?1000l\033[?7h\033[?25h"

// Alternate screen first (it has no scrollback of its own), then the modes above.
static void enter_screen() {
    printf("\033[?1049h" TERM_MODES);
    fflush(stdout);
    screen_active = 1;
}

void restore_screen() {
    if (screen_active) {
        printf("\033[0m" TERM_MODES_OFF "\033[?1049l");
        fflush(stdout);
        screen_active = 0;
    }
}

void cleanup() {
    restore_screen();
    restore_input();
}

// Everything typed, clicked or scrolled is read and thrown away, so it never piles up in the tty queue.
static void drain_input() {
    struct pollfd p = {STDIN_FILENO, POLLIN, 0};
    char buf[256];
    while (poll(&p, 1, 0) > 0 && (p.revents & POLLIN)) {
        if (read(STDIN_FILENO, buf, sizeof buf) <= 0) {
            break;
        }
    }
}

static long now_ms() {
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (long)ts.tv_sec * 1000 + ts.tv_nsec / 1000000;
}

// Sleeps up to `ms` milliseconds, but wakes up quickly once a stop was requested.
static void sleep_ms(int ms) {
    while (ms > 0 && !stop_looping) {
        int step = min(ms, 20);
        usleep(step * 1000);
        drain_input();
        ms -= step;
    }
}

// Terminals that start (or are switched) to full screen resize shortly after launch;
// wait until the size stops changing so the animation is laid out for the final size.
static void wait_for_stable_size() {
    struct winsize last, cur;
    if (ioctl(STDOUT_FILENO, TIOCGWINSZ, &last) != 0) {
        return;
    }
    int stable_ms = 0, waited_ms = 0;
    while (stable_ms < 500 && waited_ms < 4000 && !stop_looping) {
        usleep(50000);
        waited_ms += 50;
        if (ioctl(STDOUT_FILENO, TIOCGWINSZ, &cur) != 0) {
            return;
        }
        if (cur.ws_row == last.ws_row && cur.ws_col == last.ws_col) {
            stable_ms += 50;
        } else {
            stable_ms = 0;
            last = cur;
        }
    }
}

// ---------------------------------------------------
// Frame buffer: a whole frame is composed in memory and written at once, always with absolute cursor positions,
// so nothing depends on where the cursor happened to be (resize, minimize, restore, ...).

static char *fb = NULL;
static size_t fb_len = 0, fb_cap = 0;

static void fb_put(const char *s, size_t n) {
    if (fb_len + n + 1 > fb_cap) {
        fb_cap = (fb_len + n + 1) * 2;
        fb = realloc(fb, fb_cap);
        if (fb == NULL) {
            exit(1);
        }
    }
    memcpy(fb + fb_len, s, n);
    fb_len += n;
}

static void fb_str(const char *s) {
    fb_put(s, strlen(s));
}

static void fb_move(int row, int col) {
    char t[32];
    int n = snprintf(t, sizeof t, "\033[%d;%dH", row + 1, col + 1);
    fb_put(t, (size_t)n);
}

static void fb_spaces(int n) {
    static const char sp[] = "                                ";  // 32 spaces
    while (n > 0) {
        int k = min(n, 32);
        fb_put(sp, (size_t)k);
        n -= k;
    }
}

static void fb_flush() {
    if (fb_len > 0) {
        fwrite(fb, 1, fb_len, stdout);
        fflush(stdout);
        fb_len = 0;
    }
}

// ---------------------------------------------------

static int scr_rows = 0, scr_cols = 0;

// Re-reads the terminal size. Returns 1 when the screen must be repainted from scratch
// (size changed, SIGWINCH or SIGCONT arrived); the clear sequence is queued in the frame buffer.
static int sync_size() {
    struct winsize ws;
    int rows = 24, cols = 80;
    if (ioctl(STDOUT_FILENO, TIOCGWINSZ, &ws) == 0 && ws.ws_row > 0 && ws.ws_col > 0) {
        rows = ws.ws_row;
        cols = ws.ws_col;
    }
    if (rows != scr_rows || cols != scr_cols || need_redraw) {
        scr_rows = rows;
        scr_cols = cols;
        need_redraw = 0;
        // Re-assert the modes too: after a minimize/restore the terminal may have forgotten them.
        fb_str(TERM_MODES "\033[2J");
        return 1;
    }
    return 0;
}

static int size_ok() {
    return scr_cols >= PIC_W && scr_rows >= PIC_H;
}

// Paints `n` characters of `text` starting at (`row`, `col`), clearing the rest of that row.
static void draw_at(const char *text, int n, int row, int col) {
    int len = (int)strlen(text);
    int shown = min(min(n, len), scr_cols > col ? scr_cols - col : 0);
    fb_move(row, 0);
    fb_str("\033[2K");
    fb_move(row, col);
    fb_put(text, (size_t)shown);
}

// Same, centered on `row`.
static void draw_centered(const char *text, int n, int row) {
    int len = (int)strlen(text);
    draw_at(text, n, row, (scr_cols - min(len, scr_cols)) / 2);
}

// Shown instead of the scene while the terminal is smaller than the picture; vanishes by itself once it fits.
static void draw_notice() {
    char l1[96], l2[96];
    snprintf(l1, sizeof l1, "Make this terminal bigger to enter the lodge");
    snprintf(l2, sizeof l2, "needed %dx%d, now %dx%d", PIC_W, PIC_H, scr_cols, scr_rows);
    int mid = scr_rows / 2;
    for (int r = 0; r < scr_rows; ++r) {
        if (r == mid - 1) {
            draw_centered(l1, (int)strlen(l1), r);
        } else if (r == mid) {
            draw_centered(l2, (int)strlen(l2), r);
        } else {
            fb_move(r, 0);
            fb_spaces(scr_cols);
        }
    }
}

// ---------------------------------------------------
// The scene is an endless canvas. The hand-drawn picture sits in the middle (scene coordinates x 0..PIC_W-1,
// y 0..PIC_H-1); everything around it is generated from the coordinates alone, so the centre never changes
// when the terminal is resized, the scene just continues further out.

#define RIVER_ROW_FIRST 25        // river rows (animated), framed by shore rows 24 and 29
#define RIVER_ROW_LAST 28
#define SHORE_UP 24
#define SHORE_DOWN 29
#define MAX_PEAKS 1024

// Everything that moves over the scene (sun, moon, clouds, snow, birds, butterflies, fish, the sheep) is tied to the
// picture, not to the window: positions are scene columns/rows, so resizing or zooming the window only shows more or
// less of the same world. The world is WORLD_W columns wide, centred on the picture; SKY_TOP rows lie above it.
#define WORLD_W 320
#define WORLD_X0 ((PIC_W - WORLD_W) / 2)
#define SKY_TOP 40

static int line_len[LINES];       // real length of every picture row (the rest is padding)
static char *bg = NULL;           // static background, one cell per screen cell
static unsigned char *skym = NULL; // 1 = sky cell (stars may appear here)
static int bg_rows = 0, bg_cols = 0;
static unsigned char *occ = NULL;  // 1 = a tree stands here: bushes and stones keep away from it
static int g_mark_occ = 0;         // while set, put_ext() also marks the cells it paints in occ
static short *zb = NULL;           // for the cells in occ: the scene row of the foot of the object that stands there (-1: none)
static int g_zbase = 0;            // the foot row of the object being drawn now

// The trees of the lower bank decide whether they fit among the farther ones from this map of the whole scene (not of the
// screen), so which trees stand where never depends on the size of the window. g_dry: draw_*() only fill this map.
#define WOCC_X0 (-460)
#define WOCC_W 1100
#define WOCC_H 170
static unsigned char wocc[WOCC_H * WOCC_W];
static int g_dry = 0;

// Depth: a moving thing (cat, sheep) with the foot on scene row `base` is hidden by a still object whose foot is lower
// on the screen (nearer to us) and stands in front of it; on the same row or above, the moving thing is in front.
static int hidden_at(int x, int y, int base) {
    return zb != NULL && x >= 0 && x < scr_cols && y >= 0 && y < scr_rows && zb[(size_t)y * scr_cols + x] > base;
}

static unsigned hash2(int x, int y, unsigned salt) {
    unsigned h = (unsigned)x * 374761393u + (unsigned)y * 668265263u + salt * 2246822519u;
    h = (h ^ (h >> 13)) * 1274126177u;
    return h ^ (h >> 16);
}

// Seasons. All four amounts are 0..1 and change smoothly with the date (see season_update), so snow creeps in and
// melts away over weeks. The hand-drawn picture itself is never touched by the seasons, only the world around it.
typedef struct { double snow, leaf, flowers, ice, foliage; int phase, key; } Season;  // phase: 0 spring, 1 summer, 2 autumn
static Season g_season = {0.0, 0.0, 0.2, 0.0, 0.0, 1, 0};
static int bg_key = -1;  // the season key the background was built for

// Snow on the ground, as a glyph, and whether this cell is covered at the moment.
static int snowy(int sx, int sy) { return hash2(sx, sy, 91) % 1000 < (unsigned)(g_season.snow * 1000.0); }
static int floor_div(int a, int b) { return a >= 0 ? a / b : -((-a + b - 1) / b); }

// Snow on the ground is an even field: long strokes of mostly ~ with some -, staggered from row to row, a rare dot.
static char snow_glyph(int sx, int sy) {
    static const char strokes[5] = {'~', '~', '~', '~', '-'};
    if (hash2(sx, sy, 95) % 1000 >= 985) return '.';  // a rare crumb
    int block = floor_div(sx + (sy % 2) * 3, 6);
    return strokes[hash2(block, sy, 92) % 5];
}

// In autumn the flowers '@' die off: the further the season, the more of them are gone (all of them by the end of it).
static int flower_gone(int sx, int sy) {
    return g_season.phase == 2 && g_season.snow < 0.5 && hash2(sx, sy, 66) % 1000 >= (unsigned)(g_season.flowers * 1000.0);
}

// One cell of the generated meadow: snow in winter, lots of flowers and tall grass in summer, in autumn piles of
// fallen leaves and puddles.
static char meadow_cell(int sx, int sy) {
    if (snowy(sx, sy)) return snow_glyph(sx, sy);
    double leaf = g_season.leaf, flowers = g_season.flowers;
    if (leaf > 0.05) {
        unsigned c = hash2(sx, sy, 93) % 100;
        if (hash2(sx / 6, sy / 3, 98) % 100 < (unsigned)(3.0 * leaf) + 1 && c < 70) {
            return hash2(sx, sy, 99) % 4 ? '~' : '%';  // a rare pile of leaves far from the trees (they lie under the trees: see place_litter)
        }
        int m = ((sx % 9) + 9) % 9;
        if (hash2(sx / 9, sy, 88) % 100 < (unsigned)(10.0 * leaf) && m >= 1 && m <= 7) return '_';  // a puddle
        if (c < (unsigned)(4.0 * leaf)) return '~';
    }
    if (hash2(sx, sy, 1) % 1000 < (unsigned)((12.0 + 90.0 * flowers) * (1.0 - g_season.snow))) {  // none under the snow
        unsigned k = hash2(sx, sy, 89) % 100;
        return k < 60 ? '@' : (k < 85 ? '*' : 'o');
    }
    unsigned g = hash2(sx, sy, 87) % 100;
    if (g < (unsigned)(45.0 * flowers)) return g % 3 == 0 ? ';' : (g % 3 == 1 ? '"' : '\'');  // tall summer grass
    return ',';
}

static int ground_top(int sx) { return sx < PIC_W / 2 ? 14 : 12; }  // first grass row, left / right of the picture
static int mount_base(int sx) { return sx < PIC_W / 2 ? 13 : 11; }  // row the outer mountains stand on

// Cells of the hand-drawn picture; the river and its shores are generated so they can run across the whole screen.
static int is_art(int sx, int sy) {
    if (sy < 0 || sy >= PIC_H || sx < 0 || sx >= PIC_W) return 0;
    if (sy >= SHORE_UP && sy <= SHORE_DOWN) return 0;
    return sx < line_len[sy];
}

static char river_char(int sy, long k) {
    unsigned h = hash2((int)(k % 1000003), sy, 3) % 100;
    return h < 60 ? '^' : (h < 97 ? '_' : ' ');
}

static void gen_cell(int sx, int sy, char *ch, unsigned char *sky) {
    *ch = ' ';
    *sky = 0;
    if (sy >= PIC_H) {  // near bank in front of the river: the same meadow as above
        *ch = meadow_cell(sx, sy);
    } else if (sy == SHORE_UP || sy == SHORE_DOWN) {
        *ch = '`';
    } else if (sy >= RIVER_ROW_FIRST && sy <= RIVER_ROW_LAST) {
        *ch = ' ';  // painted per frame
    } else if (sy >= ground_top(sx)) {
        if ((sy == 20 || sy == 21) && sx >= PIC_W - 2) {
            *ch = '_';  // the bank line of the picture goes on to the right
        } else {
            *ch = meadow_cell(sx, sy);
        }
    } else {
        *sky = 1;
    }
}

// Puts a generated (non-picture) cell on the screen.
static void put_ext(int ox, int oy, int sx, int sy, char ch, int solid) {
    if (g_dry) {
        if (g_mark_occ && sx >= WOCC_X0 && sx < WOCC_X0 + WOCC_W && sy >= 0 && sy < WOCC_H) wocc[sy * WOCC_W + (sx - WOCC_X0)] = 1;
        return;
    }
    int x = sx + ox, y = sy + oy;
    if (x < 0 || x >= scr_cols || y < 0 || y >= scr_rows || is_art(sx, sy)) return;
    bg[(size_t)y * scr_cols + x] = ch;
    if (solid) skym[(size_t)y * scr_cols + x] = 0;
    if (g_mark_occ && occ != NULL) {
        occ[(size_t)y * scr_cols + x] = 1;
        zb[(size_t)y * scr_cols + x] = (short)g_zbase;
    }
}

// `layer` 0 is the farthest range (sparse), 2 the nearest one (dense): the nearer a range is, the more `^` it has.
static void draw_mountain(int ox, int oy, int cx, int h, int base, int layer) {
    int top = base - h;
    for (int r = top; r <= base; ++r) {
        int d = r - top, l = cx - d, rr = cx + 1 + d;
        for (int x = l + 1; x < rr; ++x) {
            char ch = hash2(x, r, 7 + layer) % 100 < (unsigned)(14 + 14 * layer) ? '^' : ' ';
            if (d >= 1 && d <= (int)(g_season.snow * (h / 2.0) + 0.5)) {  // the snow cap of the peak
                unsigned q = hash2(x, r, 94) % 100;
                ch = q < 55 ? '*' : (q < 80 ? '.' : ' ');
            }
            put_ext(ox, oy, x, r, ch, 1);
        }
        put_ext(ox, oy, l, r, '/', 1);
        put_ext(ox, oy, rr, r, '\\', 1);
    }
}

static void draw_tree(int ox, int oy, int cx, int h, int base) {
    g_mark_occ = 1;
    g_zbase = base;
    for (int i = 0; i < h; ++i) {
        int r = base - h + i, l = cx - 1 - i, rr = cx + 1 + i;
        for (int x = l + 1; x < rr; ++x) {
            char ch = (i >= 1 && hash2(x, r, 9) % 100 < 20) ? 'o' : '|';
            put_ext(ox, oy, x, r, ch, 1);
        }
        put_ext(ox, oy, l, r, '/', 1);
        put_ext(ox, oy, rr, r, '\\', 1);
    }
    put_ext(ox, oy, cx, base - h - 1, '*', 1);   // the star on the top
    for (int x = cx - h; x <= cx + h; ++x) put_ext(ox, oy, x, base, ' ', 1);  // the foot row is empty, like in the picture:
    put_ext(ox, oy, cx - h, base, '\'', 1);       // nothing grows between the marks and inside the trunk
    put_ext(ox, oy, cx - 1, base, '|', 1);
    put_ext(ox, oy, cx + 1, base, '|', 1);
    put_ext(ox, oy, cx + h, base, '\'', 1);
    g_mark_occ = 0;
}

// Deciduous trees, so that the foliage can change with the seasons: bare in winter, blossoms in spring, dense in summer,
// yellowing and thinning in autumn. `foliage` (0..1) says how many cells of the crown carry leaves.
typedef struct { int cx, top, ch, base; } Crown;  // scene coordinates; the crown has `ch` rows
#define MAX_CROWNS 2048
static Crown g_crowns[MAX_CROWNS];
static int g_ncrowns = 0;

static int crown_half(int ch, int i) {  // half width of the row i of the crown of the template tree
    static const int hw[6] = {4, 6, 7, 7, 6, 4};
    (void)ch;
    return hw[i < 0 ? 0 : (i > 5 ? 5 : i)];
}

// The leaf of a crown cell, by season.
static char leaf_glyph(int x, int r) {
    unsigned w = hash2(x, r, 121) % 100;
    if (g_season.phase == 0) return w < 45 ? '*' : (w < 75 ? 'o' : '.');                   // blossoms
    if (g_season.phase == 2) return w < 30 ? '&' : (w < 60 ? '~' : (w < 80 ? '%' : ','));  // autumn leaves
    return w < 40 ? '#' : (w < 70 ? '&' : (w < 90 ? '%' : '@'));                           // summer: dense
}

// The leafy tree is drawn from a template, 15 cells wide, the axis of the trunk is column 7 (counted from 0).
// Summer: a round crown with an outline and branches inside; the trunk goes straight down, without roots.
// Winter: the same trunk and branches, no outline.
// Spring and autumn: the summer tree with blossoms or yellow leaves between the branches.
#define OAK_ROWS 8
static const char *const OAK_CROWN[OAK_ROWS] = {
    "   .-------.   ",
    " /  \\  |  /  \\ ",
    "(  \\ \\ | / /  )",
    "(   \\ \\|/ /   )",
    " \\   -\\|/-   / ",
    "   `---|---'   ",
    "       |       ",
    "       |       ",
};
static const char *const OAK_BARE[OAK_ROWS] = {
    "               ",
    "    \\  |  /    ",
    "   \\ \\ | / /   ",
    "    \\ \\|/ /    ",
    "     -\\|/-     ",
    "       |       ",
    "       |       ",
    "       |       ",
};

// `base` is the row of the foot of the trunk. Around the tree there is a blank margin one cell wide that follows its silhouette
// (the span of every row), so that the grass, the snow or the mountains behind it do not mix with the drawing.
static void draw_oak(int ox, int oy, int cx, int base) {
    g_mark_occ = 1;
    g_zbase = base;
    int top = base - (OAK_ROWS - 1);
    const char *const *tpl = g_season.foliage > 0.05 ? OAK_CROWN : OAK_BARE;
    for (int i = 0; i < OAK_ROWS; ++i) {
        int first = -1, last = -1;
        for (int k = 0; k < 15; ++k) {
            if (tpl[i][k] != ' ') {
                if (first < 0) first = k;
                last = k;
            }
        }
        for (int k = first; first >= 0 && k <= last; ++k) {
            for (int dy = -1; dy <= 1; ++dy) {
                for (int dx = -1; dx <= 1; ++dx) {
                    if (dx != 0 && dy != 0) continue;  // a cross, not a box: under the crown the margin is one cell narrower on each side
                    put_ext(ox, oy, cx - 7 + k + dx, top + i + dy, ' ', 1);
                }
            }
        }
    }
    for (int i = 0; i < OAK_ROWS; ++i) {
        for (int k = 0; k < 15; ++k) {
            if (tpl[i][k] != ' ') put_ext(ox, oy, cx - 7 + k, top + i, tpl[i][k], 1);
        }
    }
    if (g_season.foliage > 0.05 && g_season.phase != 1) {  // blossoms in spring, yellow leaves in autumn, between the branches
        for (int i = 1; i <= 4; ++i) {
            int hw = crown_half(6, i);
            for (int dx = -hw + 1; dx < hw; ++dx) {
                int x = cx + dx, r = top + i;
                if (tpl[i][7 + dx] == ' ' && hash2(x, r, 120) % 1000 < (unsigned)(g_season.foliage * 550.0)) {
                    put_ext(ox, oy, x, r, leaf_glyph(x, r), 1);
                }
            }
        }
    }
    if (!g_dry && g_ncrowns < MAX_CROWNS) g_crowns[g_ncrowns++] = (Crown){cx, top, 6, base};
    g_mark_occ = 0;
}

// Small plants and stones, drawn from templates like the leafy tree: with a blank margin that follows the silhouette.
// Bushes: a round outline with branches inside (summer), bare branches (winter), leaves between the branches in spring and
// autumn. Snow mounds and stones lie on the lower bank in winter.
static const char *const BUSH_CROWN[3] = {" .-\"-. ", "( \\|/ )", " `-|-' "};
static const char *const BUSH_BARE[3] = {" \\ | / ", "  \\|/  ", "   |   "};
static const char *const SNOW_MOUND[2] = {"  _.-._  ", " (_____) "};
static const char *const STONE[2] = {" .-. ", "(___)"};

// Everything but the trees is switched off for now (set to 1 to bring it back): bushes, stones, snow mounds.
#define SHOW_BUSHES 0
#define SHOW_STONES 0
#define SHOW_MOUNDS 0

static int plant_free(int ox, int oy, int left, int base, int n, int w) {
    for (int sy = base - n; sy <= base + 1; ++sy) {
        for (int sx = left - 2; sx <= left + w + 1; ++sx) {
            int x = sx + ox, y = sy + oy;
            if (x < 0 || x >= scr_cols || y < 0 || y >= scr_rows) continue;
            if (occ[(size_t)y * scr_cols + x]) return 0;
        }
    }
    return 1;
}

// `tight`: the blank margin is a cross (left, right, up, down), without the diagonal cells, so it hugs the shape closer.
static void draw_plant(int ox, int oy, int left, int base, const char *const *rows, int n, int w, int tight) {
    int top = base - (n - 1);
    g_mark_occ = 1;
    g_zbase = base;
    for (int i = 0; i < n; ++i) {
        int first = -1, last = -1;
        for (int k = 0; k < w; ++k) {
            if (rows[i][k] != ' ') {
                if (first < 0) first = k;
                last = k;
            }
        }
        for (int k = first; first >= 0 && k <= last; ++k) {
            for (int dy = -1; dy <= 1; ++dy) {
                for (int dx = -1; dx <= 1; ++dx) {
                    if (tight && dx != 0 && dy != 0) continue;
                    put_ext(ox, oy, left + k + dx, top + i + dy, ' ', 1);
                }
            }
        }
    }
    for (int i = 0; i < n; ++i) {
        for (int k = 0; k < w; ++k) {
            if (rows[i][k] != ' ') put_ext(ox, oy, left + k, top + i, rows[i][k], 1);
        }
    }
    g_mark_occ = 0;
}

// Walks along the screen from a fixed point of the scene (so nothing moves when the window is resized) and puts a
// plant every few cells. bank = 1: the lower bank under the river; bank = 0: the lower edge of the meadow.
static void place_plants(int ox, int oy, int bank) {
    int x = -420, x_end = scr_cols - ox + 12, x_vis = -ox - 14;
    int leafy = g_season.foliage > 0.05;
    int winter = g_season.snow >= 0.5;
    for (int k = 0; k < 4000 && x < x_end; ++k) {
        unsigned h1 = hash2(k, bank, 151), h2 = hash2(k, bank, 152), h3 = hash2(k, bank, 153);
        int kind = bank ? (h1 % 100 < 40 ? 0 : (h1 % 100 < 75 ? 1 : 2)) : (h1 % 100 < 35 ? 0 : 3);  // 0 bush, 1 mound, 2 stone, 3 nothing
        if (kind >= 1 && kind <= 2 && !winter) kind = 3;
        if ((kind == 0 && !SHOW_BUSHES) || (kind == 1 && !SHOW_MOUNDS) || (kind == 2 && !SHOW_STONES)) kind = 3;
        int w = kind == 0 ? 7 : (kind == 1 ? 9 : 5);
        int base = bank ? 32 + (int)(h2 % 5) : 23;
        int gap = bank ? 12 + (int)(h3 % 30) : 8 + (int)(h3 % 22);
        if (kind != 3 && x + w >= x_vis && plant_free(ox, oy, x, base, kind == 0 ? 3 : 2, w)) {
            if (kind == 0) {
                draw_plant(ox, oy, x, base, leafy ? BUSH_CROWN : BUSH_BARE, 3, 7, 1);
                if (leafy && g_season.phase != 1) {  // blossoms or yellow leaves between the branches
                    for (int dx = 1; dx <= 5; dx += 4) {
                        int lx = x + dx, ly = base - 1;
                        if (hash2(lx, ly, 120) % 1000 < (unsigned)(g_season.foliage * 550.0)) put_ext(ox, oy, lx, ly, leaf_glyph(lx, ly), 1);
                    }
                }
            } else {
                draw_plant(ox, oy, x, base, kind == 1 ? SNOW_MOUND : STONE, 2, w, 0);
            }
        }
        x += w + gap;
    }
}

// Trees on the whole lower bank under the river, band after band: the first band stands right under the river, every
// next one 8 rows lower, as far as the bottom of the screen, so a taller window just shows more of the forest. The nearer
// the band, the taller its pines. Every band is a walk through the scene from a fixed point (so a resize moves nothing);
// the bands are drawn from the farthest to the nearest, so a nearer tree stands in front of a farther one.
static void place_bank_trees(int ox, int oy) {
    const int x_end = 560;  // the walk does not depend on the window: only what is on the screen gets drawn
    int x_vis = -ox - 16, x_scr = scr_cols - ox + 14;
    memset(wocc, 0, sizeof wocc);
    for (int band = 0; band < 14; ++band) {
        if (36 + 8 * band - 9 >= scr_rows - oy) break;  // the tops of this band are already below the screen
        int x = -420;
        for (int k = 0; k < 4000 && x < x_end; ++k) {
            unsigned h1 = hash2(k, band, 161), h2 = hash2(k, band, 162), h3 = hash2(k, band, 163);
            int oak = h1 % 100 < 45;
            int h = oak ? 6 : 5 + (int)(h2 % 4) + (band > 4 ? 4 : band);
            int reach = oak ? 9 : h + 1;
            int base = 36 + 8 * band + (int)(h3 % 7);
            int cx = x + reach;
            // The box of the tree: a pine is a triangle with the star above it, an oak is 15 cells wide and 8 rows high.
            int x0 = oak ? cx - 8 : cx - h - 1, x1 = oak ? cx + 8 : cx + h + 1;
            int y0 = oak ? base - 8 : base - h - 2, y1 = base + 1;
            int seen = 1;
            for (int sy = y0; sy <= y1 && seen; ++sy) {
                for (int sx = x0; sx <= x1; ++sx) {
                    if (sx >= WOCC_X0 && sx < WOCC_X0 + WOCC_W && sy >= 0 && sy < WOCC_H && wocc[sy * WOCC_W + (sx - WOCC_X0)]) {
                        seen = 0;  // a tree of a farther band stands here: never draw over it, that spoils both trees
                        break;
                    }
                }
            }
            if (seen) {
                g_dry = 1;  // first into the map of the whole scene, then (if it is on the screen) for real
                if (oak) draw_oak(ox, oy, cx, base); else draw_tree(ox, oy, cx, h, base);
                g_dry = 0;
                if (cx + reach >= x_vis && cx - reach <= x_scr) {
                    if (oak) draw_oak(ox, oy, cx, base); else draw_tree(ox, oy, cx, h, base);
                }
            }
            x = cx + reach + 3 + (int)(hash2(k, band, 164) % 14);
        }
    }
}

// Chains of mountains / rows of trees marching outwards from the picture. side = -1 (left) or +1 (right).
static void build_side(int ox, int oy, int side, int extent) {
    // Three ranges, from the farthest to the nearest: tall peaks far away, hills in front. Peaks of one range overlap
    // each other and the nearer range stands in front of the farther one, so the slopes are layered, not in a row.
    static const int h_min[3] = {8, 5, 3}, h_span[3] = {5, 5, 4}, overlap[3] = {3, 3, 1};
    for (int layer = 0; layer < 3; ++layer) {
        int cxs[MAX_PEAKS], hs[MAX_PEAKS], n = 0;
        int edge = side < 0 ? -3 - 5 * layer : PIC_W + 2 + 5 * layer;
        for (int k = 0; k < MAX_PEAKS && (side < 0 ? edge > -extent - 24 : edge < PIC_W + extent + 24); ++k) {
            int h = h_min[layer] + (int)(hash2(k, side * 3 + layer, 21) % (unsigned)h_span[layer]);
            int gap = (int)(hash2(k, side * 3 + layer, 22) % (unsigned)(overlap[layer] + 2)) - overlap[layer];  // negative: overlaps
            if (k == 0 && gap < 0) gap = 0;                   // the first peak never reaches into the picture
            int cx = side < 0 ? edge - gap - h : edge + gap + h;
            cxs[n] = cx;
            hs[n++] = h;
            edge = side < 0 ? cx - h : cx + 1 + h;
        }
        for (int k = n - 1; k >= 0; --k) {
            draw_mountain(ox, oy, cxs[k], hs[k], mount_base(side < 0 ? -1 : PIC_W), layer);
        }
    }

    int edge = side < 0 ? -3 : PIC_W + 2;
    for (int k = 0; k < MAX_PEAKS && (side < 0 ? edge > -extent - 24 : edge < PIC_W + extent + 24); ++k) {
        int oak = hash2(k, side, 34) % 100 < 45;  // about every other tree is deciduous
        int h = oak ? 6 : 4 + (int)(hash2(k, side, 31) % 4);
        int reach = oak ? 9 : h + 1;              // half of the width of the tree with its margin
        // The leafy trees stand nearer to us than the pines, lower on the meadow, so their round crowns have grass or
        // snow behind them and not the slopes of the mountains.
        int base = oak ? 21 + (int)(hash2(k, side, 32) % 2) : 16 + (int)(hash2(k, side, 32) % 3);
        int cx = side < 0 ? edge - reach : edge + reach;
        if (oak) draw_oak(ox, oy, cx, base); else draw_tree(ox, oy, cx, h, base);
        int space = 2 + (int)(hash2(k, side, 33) % 7);
        edge = side < 0 ? cx - reach - space : cx + reach + space;
    }
}

// The cat (see cat_pose) sits by the hut all year round, in a hat while the river is frozen. In winter a snowman stands
// on the meadow too. The snowman gets a blank margin (the silhouette widened by one cell, up/down/left/right only).
#define CAT_SX 6                 // scene x of the cat's left edge at home, its lowest row is scene row CAT_BASE
#define CAT_BASE 20
#define MAN_SX 62                // right edge of the meadow, between the last pines and the picture's edge
#define MAN_BASE 18
static const char *const MAN_ROWS[] = {"  _[_]_  ", "  (o,o)  ", ">-(   )-<", " (  :  ) ", "  `---'  "};

static void stamp_pet(int ox, int oy, int sx, int base, const char *const *rows, int n, int mark) {
    int top = base - n + 1;
    for (int i = 0; i < n; ++i) {  // blank margin first, then the drawing over it
        int lo = 1 << 20, hi = -1;
        for (int k = 0; rows[i][k] != '\0'; ++k) {
            if (rows[i][k] != ' ') {
                if (k < lo) lo = k;
                hi = k;
            }
        }
        for (int k = lo; k <= hi; ++k) {
            static const int d[5][2] = {{0, 0}, {1, 0}, {-1, 0}, {0, 1}, {0, -1}};
            for (int j = 0; j < 5; ++j) {
                int x = sx + k + d[j][0] + ox, y = top + i + d[j][1] + oy;
                if (x >= 0 && x < scr_cols && y >= 0 && y < scr_rows) {
                    bg[(size_t)y * scr_cols + x] = ' ';
                    skym[(size_t)y * scr_cols + x] = 0;
                    if (mark) {
                        occ[(size_t)y * scr_cols + x] = 1;
                        zb[(size_t)y * scr_cols + x] = (short)base;
                    }
                }
            }
        }
    }
    for (int i = 0; i < n; ++i) {
        for (int k = 0; rows[i][k] != '\0'; ++k) {
            int x = sx + k + ox, y = top + i + oy;
            if (rows[i][k] != ' ' && x >= 0 && x < scr_cols && y >= 0 && y < scr_rows) {
                bg[(size_t)y * scr_cols + x] = rows[i][k];
            }
        }
    }
}

static void place_winter_pets(int ox, int oy) {
    if (g_season.snow < 0.5) return;
    int n_man = (int)(sizeof MAN_ROWS / sizeof *MAN_ROWS);
    if (MAN_BASE + 1 + oy < scr_rows && MAN_BASE - n_man - 1 + oy >= 0) {  // only when the whole snowman fits
        stamp_pet(ox, oy, MAN_SX, MAN_BASE, MAN_ROWS, n_man, 1);
    }
}

// The cat shuts its eyes for a quarter of a second every 4-8 seconds or so, at uneven moments.
static int cat_blinks(long t_ms) {
    long slot = t_ms / 6000;
    long start = 1000 + (long)(hash2((int)slot, 7, 301) % 4000);
    long in = t_ms - slot * 6000;
    return in >= start && in < start + 250;
}

// Fallen leaves lie mostly under the leafy trees, round the foot of the trunk: the nearer to the trunk, the thicker.
// The litter only grows through the autumn (the crowns lose their leaves) and stays until the snow covers it, while the
// leaves still falling (g_season.leaf) die away at the end of the season.
static void place_litter(int ox, int oy) {
    if (g_season.phase != 2 || g_season.snow >= 0.5) return;
    double litter = (1.0 - g_season.foliage) / 0.9;  // 0 at the start of autumn, 1 when the crowns are bare
    litter = litter < 0.0 ? 0.0 : (litter > 1.0 ? 1.0 : litter);
    if (litter < 0.03) return;
    double rx = 5.0 + 6.0 * litter, ry = 2.0 + 2.0 * litter;  // the litter spreads round the trunk as it grows
    for (int i = 0; i < g_ncrowns; ++i) {
        const Crown *c = &g_crowns[i];
        for (int dy = 0; dy <= 4; ++dy) {
            for (int dx = -11; dx <= 11; ++dx) {
                if (dy == 0 && dx >= -1 && dx <= 1) continue;  // the trunk itself
                int x = c->cx + dx + ox, y = c->base + dy + oy;
                if (x < 0 || x >= scr_cols || y < 0 || y >= scr_rows || is_art(x - ox, y - oy)) continue;
                size_t k = (size_t)y * scr_cols + x;
                if (occ[k] && bg[k] != ' ') continue;  // not over a tree
                double ex = dx / rx, ey = dy / ry, d2 = ex * ex + ey * ey;
                if (d2 >= 1.0) continue;
                if ((double)(hash2(x - ox, y - oy, 150) % 1000) / 1000.0 < (1.0 - d2) * (0.3 + 0.65 * litter)) {
                    bg[k] = hash2(x - ox, y - oy, 151) % 4 ? '~' : '%';
                }
            }
        }
    }
}

static void build_background() {
    size_t n = (size_t)scr_rows * (size_t)scr_cols;
    bg = realloc(bg, n);
    skym = realloc(skym, n);
    occ = realloc(occ, n);
    zb = realloc(zb, n * sizeof *zb);
    if (bg == NULL || skym == NULL || occ == NULL || zb == NULL) {
        exit(1);
    }
    memset(occ, 0, n);
    memset(zb, 0xFF, n * sizeof *zb);
    bg_rows = scr_rows;
    bg_cols = scr_cols;
    bg_key = g_season.key;
    g_ncrowns = 0;
    int ox = (scr_cols - PIC_W) / 2, oy = (scr_rows - PIC_H) / 2;

    for (int y = 0; y < scr_rows; ++y) {
        for (int x = 0; x < scr_cols; ++x) {
            size_t i = (size_t)y * scr_cols + x;
            int sx = x - ox, sy = y - oy;
            if (is_art(sx, sy)) {
                char a = lines[sy][sx];
                if (a == '@' && g_season.snow >= 0.5) {
                    a = '*';                                       // flowers under the snow
                } else if (a == ',' && sy >= 12 && snowy(sx, sy)) {
                    a = snow_glyph(sx, sy);                        // the meadow of the picture gets the same snow as the rest
                } else if (a == '@' && flower_gone(sx, sy)) {
                    a = meadow_cell(sx, sy);                       // an autumn flower of the picture has withered
                } else if (a == ',' && sy >= 12 && g_season.phase == 1 && g_season.snow < 0.5) {
                    a = meadow_cell(sx, sy);                       // and in summer the same tall grass and flowers
                }
                bg[i] = a;
                skym[i] = 0;
            } else {
                gen_cell(sx, sy, &bg[i], &skym[i]);
            }
        }
    }

    // Blank cells above the first drawn character of a picture column are open sky.
    for (int sx = 0; sx < PIC_W; ++sx) {
        int first = 13;
        for (int sy = 0; sy <= 12; ++sy) {
            if (is_art(sx, sy) && lines[sy][sx] != ' ') {
                first = sy;
                break;
            }
        }
        for (int sy = 0; sy < first; ++sy) {
            int x = sx + ox, y = sy + oy;
            if (is_art(sx, sy) && x >= 0 && x < scr_cols && y >= 0 && y < scr_rows) {
                skym[(size_t)y * scr_cols + x] = 1;
            }
        }
    }

    build_side(ox, oy, -1, ox);
    build_side(ox, oy, +1, scr_cols - ox - PIC_W);
    place_plants(ox, oy, 0);   // bushes at the lower edge of the meadow (switched off for now)
    place_plants(ox, oy, 1);   // snow mounds on the lower bank, and bushes and stones when they are switched on:
                               // they go first, and the forest of the bank leaves them their place
    place_winter_pets(ox, oy); // the snowman marks its place before the forest of the bank is planted
    place_bank_trees(ox, oy);  // trees on the lower bank
    place_litter(ox, oy);      // autumn: the fallen leaves lie under the leafy trees
}

static double g_star_f = 1.0;  // 0 = broad daylight, 1 = night: how many of the stars are out

// Calm night sky: most stars are steady dots, one in eight slowly brightens for two seconds every 10-16 seconds,
// a few are bright and steady. Nothing blinks out, and the stars do not twinkle in unison.
static char star_char(int sx, int sy, long t_ms) {
    if (hash2(sx, sy, 41) % 1000 >= (unsigned)(40.0 * g_star_f)) {
        return ' ';
    }
    unsigned kind = hash2(sx, sy, 42) % 16;
    if (kind < 2) {
        long period = 10000 + (long)(hash2(sx, sy, 43) % 4) * 2000;
        long at = (t_ms + (long)(hash2(sx, sy, 44) % 16000)) % period;
        return at < 2000 ? '+' : '.';
    }
    return kind == 2 ? '*' : '.';
}

// A fish now and then jumps out of the river to the right and dives back in. At most one jump per 9-second window,
// placed at a random spot and time inside it; everything is derived from the time, so no state is needed.
#define FISH_WINDOW_MS 9000
#define FISH_EVENT_MS 2300
#define FISH_DX 14           // how far to the right a jump carries the fish
#define FISH_RIGHT "><(((o>"  // the fish swimming right; ASCII only, so one byte is one cell
#define FISH_LEFT "<o)))><"   // and the mirrored one

typedef struct { int active; long dt; int x0; int row0; int h; int dir; } Fish;

// Plans the jump of window k: where it starts and which way it goes. Returns 0 when the window has no jump (or the
// river is frozen). A "hut jump" takes place under the hut, where the cat can run down to the shore to watch it.
static int fish_plan(int k, Fish *f, long *start, int *hut) {
    *f = (Fish){0, 0, 0, 0, 3, 1};
    if (g_season.ice >= 0.5) {
        return 0;  // the river is frozen over
    }
    if (!g_demo && hash2(k, 0, 51) % 100 >= 15) {
        return 0;  // only 15 windows out of 100 have a jump: about one a minute
    }
    *hut = hash2(k, 0, 56) % 100 < 40;
    *start = (long)k * FISH_WINDOW_MS + 1500 + (long)(hash2(k, 0, 52) % (*hut ? 2000 : 5500));  // hut jumps start early: the cat must run and come back inside the window
    f->dir = hash2(k, 0, 55) % 2 ? 1 : -1;                   // left or right, equally likely
    f->row0 = RIVER_ROW_LAST - (int)(hash2(k, 0, 54) % 2);  // takes off from one of the two lower river rows

    // Scene columns. A tall jump needs open grass on one side of the picture, so the fish never crosses the hut and the
    // trees; a low jump may start anywhere along the river. A left jump starts FISH_DX further right.
    unsigned pick = hash2(k, 0, 53);
    int base;
    if (*hut) {
        base = 2 + (int)(pick % 21);  // the whole jump stays between scene columns 2 and 37, under the hut
        f->h = 3;
    } else if (pick % 3 == 0) {
        base = -42 + (int)((pick / 3) % 30);   // open grass left of the picture
        f->h = 6;
    } else if (pick % 3 == 1) {
        base = PIC_W + 4 + (int)((pick / 3) % 30);  // and right of it
        f->h = 6;
    } else {
        base = -40 + (int)((pick / 3) % (unsigned)(PIC_W + 80 - FISH_DX));
        f->h = 3;
    }
    f->x0 = f->dir > 0 ? base : base + FISH_DX;
    return 1;
}

static Fish fish_at(long t_ms) {
    t_ms += g_ev_off;
    Fish f;
    long start;
    int hut;
    if (!fish_plan((int)(t_ms / FISH_WINDOW_MS), &f, &start, &hut)) {
        return (Fish){0, 0, 0, 0, 3, 1};
    }
    long dt = t_ms - start;
    if (dt < 0 || dt >= FISH_EVENT_MS) {
        f.active = 0;
        return f;
    }
    f.active = 1;
    f.dt = dt;
    return f;
}

#define MAX_SPRITES 12
// knock: 0 = plain, 1 = clear one cell of margin around it on its row, 2 = also clear the rows above and below.
typedef struct { int x, y; const char *s; int knock; } Sprite;

static int arc_x(const Fish *f, double v) { return f->x0 + f->dir * (int)(FISH_DX * v + 0.5); }
static int arc_y(const Fish *f, double v) { return f->row0 - (int)(f->h * 4.0 * v * (1.0 - v) + 0.5); }

static int add_centered(Sprite *out, int n, int cx, int y, const char *s, int knock) {
    out[n] = (Sprite){cx - (int)(strlen(s) / 2), y, s, knock};
    return n + 1;
}

// What to draw for the jump: rings at take-off, the fish with a trail of drops, a splash and rings where it dives.
static int fish_sprites(const Fish *f, int oy, Sprite *out) {
    int n = 0;
    if (!f->active) {
        return 0;
    }
    static const char *const rings[] = {".o.", "(o)", "( o )", "(   )"};
    int land_x = f->x0 + f->dir * FISH_DX;

    if (f->dt < 1000) {
        n = add_centered(out, n, f->x0, oy + f->row0, rings[f->dt / 250], 1);
    }
    if (f->dt >= 1350 && f->dt < 2350) {
        n = add_centered(out, n, land_x, oy + f->row0, rings[(f->dt - 1350) / 250], 1);
    }
    if (f->dt >= 1350 && f->dt < 1650) {
        n = add_centered(out, n, land_x, oy + f->row0 - 1, "\\|/", 0);
    }
    if (f->dt >= 100 && f->dt < 1500) {
        double v = (double)(f->dt - 100) / 1400.0;
        static const char *const drops[] = {".", "'", "."};
        for (int d = 0; d < 3; ++d) {  // drops left behind along the path
            double vd = v - 0.07 * (d + 1);
            if (vd > 0.0) {
                n = add_centered(out, n, arc_x(f, vd) - f->dir * 4, oy + arc_y(f, vd) + 1, drops[d], 0);
            }
        }
        n = add_centered(out, n, arc_x(f, v), oy + arc_y(f, v), f->dir > 0 ? FISH_RIGHT : FISH_LEFT, 0);
    }
    return n;
}

// The cat. It sits by the hut (in a hat while the river is frozen). When a fish jumps under the hut it runs down to
// the shore (first down, then along it), watches, and goes back the same way. Everything follows from the time.
#define CAT_SHORE_BASE 23   // row the cat stands on at the shore
static const char *const CAT_HAT[] = {"  *   ", " /_\\  ", "(o.o) ", "=(w)= ", " (_)_)"};
static const char *const CAT_BARE[] = {"/\\_/\\ ", "(o.o) ", "=(w)= ", " (_)_)"};
// Running along the shore the cat is the same cat, mirrored when it runs to the right (the paws alternate between two frames).
typedef struct { int x, base, moving, dir, face; } CatPose;  // left edge, lowest row; dir: +1 runs right, -1 left, 0 up/down; face: +1 mirrored (looks right), -1 as drawn

// Where the cat runs to, in scene columns, for the hut jump `f`; and which way it runs along the shore on the way there.
static int cat_target(const Fish *f) {
    int tx = f->x0 + f->dir * (FISH_DX / 2) - 2;  // under the middle of the jump
    tx = tx < 1 ? 1 : (tx > 14 ? 14 : tx);              // the shore stretch left of the hut
    return tx == CAT_SX ? CAT_SX - 1 : tx;              // never straight down: then it would not turn
}

// The cat keeps the way it faced at its last stop: after a run to the left it sits looking left, after a run to the right
// (or the way back from a run to the left) looking right. That is where the latest finished run left it.
static int cat_last_face(int window) {
    for (int k = window; k >= window - 300 && k >= 0; --k) {
        Fish f;
        long start;
        int hut;
        if (fish_plan(k, &f, &start, &hut) && hut) {
            int out = cat_target(&f) > CAT_SX ? 1 : -1;
            return -out;  // it came back the other way
        }
    }
    return -1;
}

static CatPose cat_pose(long t_ms) {
    t_ms += g_ev_off;
    int k = (int)(t_ms / FISH_WINDOW_MS);
    CatPose c = {CAT_SX, CAT_BASE, 0, 0, 0};
    Fish f;
    long start;
    int hut;
    if (!fish_plan(k, &f, &start, &hut) || !hut) {
        c.face = cat_last_face(k);
        return c;
    }
    int tx = cat_target(&f), out = tx > CAT_SX ? 1 : -1;
    int down = CAT_SHORE_BASE - CAT_BASE, along = tx > CAT_SX ? tx - CAT_SX : CAT_SX - tx;
    int steps = down + along;
    long run = 300 + 70L * steps, t = t_ms - start - 300;  // t = 0 when the cat sets off
    long s;
    if (t < 0) {
        c.face = cat_last_face(k - 1);
        return c;
    }
    if (t >= 2 * run + 2000) {
        c.face = -out;
        return c;
    }
    if (t < run) s = t * steps / run;
    else if (t < run + 2000) s = steps;
    else s = steps - (t - run - 2000) * steps / run;
    c.moving = s > 0 && s < steps && (t < run || t >= run + 2000);
    int vy = s < down ? (int)s : down, vx = (int)s - vy;
    c.base = CAT_BASE + vy;
    c.x = CAT_SX + (tx > CAT_SX ? vx : -vx);
    if (c.moving && s > down) c.dir = out * (t < run ? 1 : -1);  // along the shore: out or back
    if (t < run) c.face = s > down || t >= run ? out : cat_last_face(k - 1);  // goes down first, still looking the old way
    else if (t < run + 2000) c.face = out;
    else c.face = -out;
    return c;
}

static void span_of(const char *r, int *lo, int *hi) {
    *lo = 1 << 20;
    *hi = -1;
    for (int k = 0; r[k] != '\0'; ++k) {
        if (r[k] != ' ') {
            if (k < *lo) *lo = k;
            *hi = k;
        }
    }
}

// Paints the cat into screen row y: a blank margin around it (up/left/right, none below), then the cat itself.
static void draw_cat(char *row, int y, int ox, int oy, long t_ms, const CatPose *c, int hat) {
    const char *const *rows = hat ? CAT_HAT : CAT_BARE;
    int n = hat ? 5 : 4, eye = hat ? 2 : 1;
    int i = y - oy - (c->base - n + 1);
    char sprite[5][16];  // the cat as drawn now: paws and mirroring applied
    for (int j = 0; j < n; ++j) {
        snprintf(sprite[j], sizeof sprite[j], "%s", rows[j]);
        if (c->moving && j == n - 1 && (t_ms / 150) % 2) snprintf(sprite[j], sizeof sprite[j], "(_)_) ");  // the paws
        if (!hat && c->face > 0) {  // looks to the right: the mirror image of the cat as drawn
            int len = (int)strlen(rows[j]);
            char m[16];
            for (int k = 0; k < len; ++k) {
                char ch = sprite[j][len - 1 - k];
                m[k] = ch == '(' ? ')' : ch == ')' ? '(' : ch == '/' ? '\\' : ch == '\\' ? '/' : ch;
            }
            m[len] = '\0';
            memcpy(sprite[j], m, (size_t)len + 1);
        }
    }
    for (int d = 0; d <= 1; ++d) {  // no margin under the cat (sprite row i+1 paints the margin above it)
        int j = i + d, lo, hi;
        if (j < 0 || j >= n) continue;
        span_of(sprite[j], &lo, &hi);
        for (int k = lo - (d == 0); k <= hi + (d == 0); ++k) {
            int x = ox + c->x + k;
            if (x >= 0 && x < scr_cols && !hidden_at(x, y, c->base)) row[x] = ' ';
        }
    }
    if (i < 0 || i >= n) return;
    if (i == eye && !c->moving && cat_blinks(t_ms)) sprite[i][1] = sprite[i][3] = '-';
    for (int k = 0; sprite[i][k] != '\0'; ++k) {
        int x = ox + c->x + k;
        if (sprite[i][k] != ' ' && x >= 0 && x < scr_cols && !hidden_at(x, y, c->base)) row[x] = sprite[i][k];
    }
}

// The sheep. It wanders along a fixed line of the lower bank (scene row SHEEP_BASE), walks to a spot, grazes there for
// a while, walks on. Everything follows from the time alone: step j is "graze at spot j, then walk to spot j+1". The
// spots are scene columns (1 unit = 1 cell, SHEEP_UNITS of them, centred on the picture), so the sheep is tied to the
// picture and does not move when the window is resized or zoomed: a narrower window only shows less of its walk. It is
// not there in winter, nor when the screen has no room for the bank under that line.
#define SHEEP_W 14
#define SHEEP_UNITS 120
#define SHEEP_BASE 35
#define SHEEP_MS_PER_UNIT 900
typedef struct { int present; double x; int base, state, face; } SheepPose;  // state: 0 walks (frame A), 1 walks (B), 2 grazes, 3 grazes (the mouth changes)

static const char *const SHEEP_BODY[3] = {"  @@@@@@@ @  @", " @@@@@@@@@/oo\\"," @@@@@@@@@\\__/"};
static const char *const SHEEP_GRAZE_BODY[3] = {"  @@@@@@@     ", " @@@@@@@@@@  @", " @@@@@@@@@/oo\\"};  // the head is one row lower

static SheepPose sheep_pose(long t_ms) {
    SheepPose sp = {0, 0.0, 0, 0, 1};
    int oy = (scr_rows - PIC_H) / 2;
    sp.base = SHEEP_BASE;  // scene row of the legs
    if (g_season.snow >= 0.2 || scr_rows - oy < SHEEP_BASE + 2) return sp;  // winter, or the bank is not on the screen
    sp.present = 1;
    long te = t_ms + g_ev_off;
    long T = 0;
    int prev = (int)(hash2(0, 0, 401) % SHEEP_UNITS);
    for (int j = 0; j < 100000; ++j) {
        int here = j == 0 ? prev : (int)(hash2(j, 0, 401) % SHEEP_UNITS);
        int next = (int)(hash2(j + 1, 0, 401) % SHEEP_UNITS);
        long graze = 8000 + (long)(hash2(j, 0, 402) % 12000);
        int dist = next > here ? next - here : here - next;
        long walk = (long)dist * SHEEP_MS_PER_UNIT;
        const double scale = 1.0, x0 = (double)(PIC_W - SHEEP_W - SHEEP_UNITS) / 2.0;  // scene column of unit 0
        if (j > 0) {
            int before = (int)(hash2(j - 1, 0, 401) % SHEEP_UNITS);
            sp.face = here >= before ? 1 : -1;  // looks the way it came
        }
        if (te < T + graze) {
            sp.x = x0 + here * scale;
            sp.state = ((te - T) / 150) % 2 ? 3 : 2;  // chews fast, like the sheep in Minecraft
            return sp;
        }
        T += graze;
        if (te < T + walk) {
            double u = (double)(te - T) / (double)walk;
            sp.x = x0 + (here + (next - here) * u) * scale;
            sp.face = next >= here ? 1 : -1;
            sp.state = ((te - T) / 400) % 2;
            return sp;
        }
        T += walk;
    }
    return sp;
}

// Paints the sheep into screen row y: a blank margin one cell wide at the sides, then the sheep.
static void draw_sheep(char *row, int y, int ox, int oy, const SheepPose *sp) {
    if (!sp->present) return;
    char sprite[4][SHEEP_W + 3];
    for (int j = 0; j < 3; ++j) snprintf(sprite[j], sizeof sprite[j], "%s", sp->state >= 2 ? SHEEP_GRAZE_BODY[j] : SHEEP_BODY[j]);
    if (sp->state == 0) snprintf(sprite[3], sizeof sprite[3], "  ||    ||    ");
    else if (sp->state == 1) snprintf(sprite[3], sizeof sprite[3], "  |/    |/    ");
    else if (sp->state == 2) snprintf(sprite[3], sizeof sprite[3], "  ||    ||\\__/");
    else snprintf(sprite[3], sizeof sprite[3], "  ||    ||\\==/");  // chewing: the mouth alternates
    if (sp->face < 0) {  // looks to the left: the mirror image
        for (int j = 0; j < 4; ++j) {
            char m[SHEEP_W + 3];
            for (int k = 0; k < SHEEP_W; ++k) {
                char ch = sprite[j][SHEEP_W - 1 - k];
                m[k] = ch == '/' ? '\\' : ch == '\\' ? '/' : ch == '(' ? ')' : ch == ')' ? '(' : ch;
            }
            m[SHEEP_W] = '\0';
            memcpy(sprite[j], m, SHEEP_W + 1);
        }
    }
    int i = y - oy - (sp->base - 3), left = ox + (int)(sp->x + (sp->x >= 0 ? 0.5 : -0.5));
    if (i < 0 || i >= 4) return;
    {  // the blank margin is only one cell at the sides of every row: none above, under the legs or under the head
        int lo, hi;
        span_of(sprite[i], &lo, &hi);
        for (int k = lo - 1; k <= hi + 1; ++k) {
            int x = left + k;
            if (x >= 0 && x < scr_cols && !hidden_at(x, y, sp->base)) row[x] = ' ';
        }
    }
    for (int k = 0; sprite[i][k] != '\0'; ++k) {
        int x = left + k;
        if (sprite[i][k] != ' ' && x >= 0 && x < scr_cols && !hidden_at(x, y, sp->base)) row[x] = sprite[i][k];
    }
}

// ---------------------------------------------------
// Wind. A gust lasts a few seconds and always blows to the same side during one run; its strength rises and falls smoothly (smoothstep ramps),
// so nothing ever jumps. Between gusts the air is still. Everything that sways reads wind_at(), which returns
// a signed strength in [-1, 1] (the sign is the direction), so leaves and clouds can use it later too.

#define WIND_WINDOW_MS 16000
#define WIND_RAMP_MS 1500

static double wabs(double x) { return x < 0 ? -x : x; }

// Triangle wave in [-1, 1] with a period of 1.0 (no libm needed).
static double tri(double x) {
    double f = x - (double)(long)x;
    if (f < 0) f += 1.0;
    return f < 0.5 ? 4.0 * f - 1.0 : 3.0 - 4.0 * f;
}

static int g_wind_dir = 1;  // +1 blows to the right, -1 to the left; chosen once per run, never changes after

static void wind_init() {
    unsigned seed = (unsigned)time(NULL) * 2654435761u ^ (unsigned)getpid();
    g_wind_dir = (seed >> 16) % 2 ? 1 : -1;
    g_ev_off = (long)((seed * 40503u >> 4) % 1200000u);  // up to 20 minutes into the pattern
    const char *sh = getenv("LODGE_EVENT_SHIFT");        // milliseconds, for tests (0 = the old fixed start)
    if (sh != NULL) g_ev_off = atol(sh);
    const char *d = getenv("LODGE_WIND_DIR");  // "left" or "right", for testing
    if (d != NULL && (d[0] == 'l' || d[0] == 'L')) g_wind_dir = -1;
    if (d != NULL && (d[0] == 'r' || d[0] == 'R')) g_wind_dir = 1;
}

// The gust of wind window k (in the shifted time of the events), if there is one.
static int wind_gust(int k, long *start, long *dur) {
    if (!g_demo && hash2(k, 0, 61) % 100 >= 30) {
        return 0;  // a calm window (3 windows out of 10 have a gust)
    }
    *start = (long)k * WIND_WINDOW_MS + 1500 + (long)(hash2(k, 0, 62) % 3000);
    *dur = 4000 + (long)(hash2(k, 0, 63) % 5000);
    return 1;
}

static double wind_at(long t_ms) {
    t_ms += g_ev_off;
    int k = (int)(t_ms / WIND_WINDOW_MS);
    long start, dur;
    if (!wind_gust(k, &start, &dur)) {
        return 0.0;
    }
    long dt = t_ms - start;
    if (dt < 0 || dt >= dur) {
        return 0.0;
    }
    double e = 1.0;
    if (dt < WIND_RAMP_MS) {
        e = (double)dt / WIND_RAMP_MS;
    } else if (dur - dt < WIND_RAMP_MS) {
        e = (double)(dur - dt) / WIND_RAMP_MS;
    }
    e = e * e * (3.0 - 2.0 * e);
    double amp = 0.6 + 0.4 * (double)(hash2(k, 0, 64) % 100) / 100.0;
    double gust = 1.0 + 0.12 * tri((double)t_ms / 2300.0);
    double w = g_wind_dir * e * amp * gust;
    return w > 1.0 ? 1.0 : (w < -1.0 ? -1.0 : w);
}

// How far a gust has pushed everything that floats on the wind so far: the integral of the gust strength over time,
// updated once per frame. Clouds, snow and leaves drift along the wind direction and are carried further by gusts.
static double g_wind_push = 0.0;
#define PUSH_RING 512                  // 51 seconds of history on a 100 ms grid
static double g_push_ring[PUSH_RING];  // the integral of the gust strength, sampled every 100 ms
static long g_push_last = 0;           // newest filled grid index
static double g_push_w = 0.0;          // the strength at that index

// The integral of the gust strength up to the moment t (seconds), from the ring; older than the ring = the oldest value.
static double wind_push_at(double t_s) {
    double g = t_s * 10.0;
    if (g < 0.0) g = 0.0;
    long oldest = g_push_last - PUSH_RING + 2;
    if (oldest < 0) oldest = 0;
    long i = (long)g;
    double f = g - (double)i;
    if (i < oldest) { i = oldest; f = 0.0; }
    if (i + 1 > g_push_last) { i = g_push_last > 0 ? g_push_last - 1 : 0; f = 1.0; }
    return g_push_ring[i % PUSH_RING] * (1.0 - f) + g_push_ring[(i + 1) % PUSH_RING] * f;
}

static void wind_push_update(long t_ms, double wind) {
    (void)wind;
    long target = t_ms / 100 + 1;
    if (target - g_push_last > 20000) g_push_last = target - 20000;  // after a long pause do not replay all of it
    while (g_push_last < target) {
        long i = ++g_push_last;
        double w = wind_at(i * 100);
        g_push_ring[i % PUSH_RING] = g_push_ring[(i - 1) % PUSH_RING] + 0.1 * 0.5 * (wabs(g_push_w) + wabs(w));
        g_push_w = w;
    }
    g_wind_push = wind_push_at((double)t_ms / 1000.0);
}

// The hand-drawn hut is never touched by the wind.
static int in_hut(int sx, int sy) {
    return (sx >= 14 && sx <= 32 && sy >= 12 && sy <= 22) || (sx >= MAN_SX - 1 && sx <= MAN_SX + 9 && sy >= MAN_BASE - 5 && sy <= MAN_BASE + 1);
}

// Bends the smoke and sways the foliage of one composed screen row. `row` is screen row y (scene row sy).
static void apply_wind(char *row, int y, int sy, int ox, double w, long t_ms) {
    int dir = w > 0 ? 1 : -1;
    double amp = wabs(w);
    size_t base = (size_t)y * scr_cols;

    if (sy >= SMOKE_BEGIN && sy <= SMOKE_END) {  // smoke: the higher, the further it is blown
        int off = (int)(w * (SMOKE_END - sy) + (w >= 0 ? 0.5 : -0.5));
        if (off != 0) {
            int xs[16], n = 0;
            for (int x = ox + 14; x <= ox + 34 && x < scr_cols && n < 16; ++x) {
                if (x >= 0 && (row[x] == '(' || row[x] == ')')) xs[n++] = x;
            }
            char glyph[16];
            for (int i = 0; i < n; ++i) {
                glyph[i] = row[xs[i]];
                row[xs[i]] = skym[base + xs[i]] ? star_char(xs[i] - ox, sy, t_ms) : ' ';
            }
            for (int i = 0; i < n; ++i) {
                int tx = xs[i] + off;
                char under = (tx >= 0 && tx < scr_cols) ? bg[base + tx] : 'x';
                int free_cell = tx >= 0 && tx < scr_cols && (under == ' ' || under == '(' || under == ')');
                row[free_cell ? tx : xs[i]] = glyph[i];
            }
        }
    }

    if (sy >= 6 && sy <= 23) {  // foliage: pine cones hop between trunk cells, the quote marks lean with the wind
        for (int x = dir > 0 ? 0 : scr_cols - 1; x >= 0 && x < scr_cols; x += dir) {
            int sx = x - ox;
            if (in_hut(sx, sy)) continue;
            double sway = 0.5 + 0.5 * tri((double)t_ms / 800.0 + (double)(hash2(sx, sy, 71) % 1000) / 1000.0);
            int tx = x + dir;
            if (tx < 0 || tx >= scr_cols) continue;
            if (row[x] == 'o' && row[tx] == '|' && amp * sway > 0.45) {
                row[x] = '|';
                row[tx] = 'o';
                x += dir;  // the moved glyph must not be moved again
            } else if (row[x] == '\'' && (row[tx] == ' ' || row[tx] == ',') && amp * (0.4 + 0.6 * sway) > 0.4) {
                row[x] = is_art(sx, sy) ? ' ' : (sy >= ground_top(sx) ? ',' : ' ');
                row[tx] = '\'';
                x += dir;
            }
        }
    }
}

// ---------------------------------------------------
// Sky. The real clock decides the time of day; the day length follows the season (northern hemisphere unless
// LODGE_HEMISPHERE=south). The sun and the moon cross the sky along an arc and are painted on sky cells only, so
// mountains and trees hide them at rise and set. Debugging: LODGE_AT="YYYY-MM-DD HH:MM" starts the clock at that
// moment, LODGE_TIME_SCALE=600 makes it run 600 times faster.

static double g_epoch0 = 0.0, g_scale = 1.0;
static int g_south = 0;
static int g_season_day = -1;  // LODGE_SEASON: a fixed day of the year instead of the date (-1 = use the date)

static void sky_init() {
    g_epoch0 = (double)time(NULL);
    const char *at = getenv("LODGE_AT");
    int y, mo, d, h = 12, mi = 0;
    if (at != NULL && sscanf(at, "%d-%d-%d %d:%d", &y, &mo, &d, &h, &mi) >= 3) {
        struct tm tmv;
        memset(&tmv, 0, sizeof tmv);
        tmv.tm_year = y - 1900;
        tmv.tm_mon = mo - 1;
        tmv.tm_mday = d;
        tmv.tm_hour = h;
        tmv.tm_min = mi;
        tmv.tm_isdst = -1;
        time_t e = mktime(&tmv);
        if (e != (time_t)-1) g_epoch0 = (double)e;
    }
    const char *sc = getenv("LODGE_TIME_SCALE");
    if (sc != NULL && atof(sc) > 0.0) g_scale = atof(sc);
    const char *hs = getenv("LODGE_HEMISPHERE");
    g_south = hs != NULL && (hs[0] == 's' || hs[0] == 'S');
    g_demo = getenv("LODGE_DEMO") != NULL;
    const char *se = getenv("LODGE_SEASON");  // winter | spring | summer | autumn: look at a season without waiting
    if (se != NULL) {
        if (se[0] == 'w' || se[0] == 'W') g_season_day = 15;
        if (se[0] == 's' && se[1] == 'p') g_season_day = 120;
        if (se[0] == 's' && se[1] == 'u') g_season_day = 200;
        if (se[0] == 'a' || se[0] == 'A') g_season_day = 292;
    }
}

static double wrap(double x, double m) {
    x -= (double)(long)(x / m) * m;
    return x < 0 ? x + m : x;
}

typedef struct { double hour, rise, set, phase; int yday; } SkyClock;

static void sky_clock(long t_ms, SkyClock *c) {
    double e = g_epoch0 + (double)t_ms / 1000.0 * g_scale;
    time_t tt = (time_t)e;
    struct tm tmv;
    localtime_r(&tt, &tmv);
    c->hour = tmv.tm_hour + tmv.tm_min / 60.0 + tmv.tm_sec / 3600.0;
    c->yday = tmv.tm_yday;
    double y = tri(((double)tmv.tm_yday - 172.0) / 365.0);      // -1 at the June solstice, +1 in December
    double summer = -(1.5 * y - 0.5 * y * y * y);               // smoothed: +1 in June, -1 in December
    if (g_south) summer = -summer;
    double len = 12.0 + 4.0 * summer;                           // 8 h in midwinter, 16 h in midsummer
    c->rise = 12.5 - len / 2.0;
    c->set = 12.5 + len / 2.0;
    c->phase = wrap((e - 947182440.0) / 86400.0, 29.530588853) / 29.530588853;  // 0 = new moon, 0.5 = full
}

static double ramp(double d, double a, double b) { return d <= a ? 0.0 : (d >= b ? 1.0 : (d - a) / (b - a)); }

// Season amounts for the date, in the northern hemisphere (the southern one is half a year later).
// Snow comes in over late November and December, stays through February and melts in March; leaves fall from
// the end of September to the end of November; flowers open in April and May and fade through September.
static void season_update(const SkyClock *c) {
    double d = g_season_day >= 0 ? (double)g_season_day : (double)((c->yday + (g_south ? 182 : 0)) % 365);
    double snow = d >= 324.0 ? ramp(d, 323.0, 354.0) : (d <= 59.0 ? 1.0 : 1.0 - ramp(d, 59.0, 90.0));
    double leaf = (d < 262.0 || d > 329.0) ? 0.0 : (d < 292.0 ? (d - 262.0) / 30.0 : (329.0 - d) / 37.0);
    double flowers = d < 91.0 ? 0.0 : (d < 135.0 ? ramp(d, 91.0, 135.0) : (d <= 243.0 ? 1.0 : 1.0 - ramp(d, 243.0, 288.0)));
    g_season.snow = snow;
    g_season.leaf = leaf;
    g_season.flowers = flowers;
    double ice = (snow - 0.45) / 0.5;  // in spring the ice goes with the snow
    ice = ice < 0.0 ? 0.0 : (ice > 1.0 ? 1.0 : ice);
    double freeze = d >= 262.0 ? ramp(d, 300.0, 342.0) : 0.0;  // in autumn the river starts to freeze in the second half of November
    g_season.ice = freeze > ice ? freeze : ice;
    // The foliage of the deciduous trees: out in April and May, full until September, thinning to the end of November.
    double foliage = d <= 91.0 ? 0.0 : (d < 135.0 ? (d - 91.0) / 44.0 : (d <= 262.0 ? 1.0 : (d < 329.0 ? 1.0 - 0.9 * (d - 262.0) / 67.0 : 0.1 * (1.0 - ramp(d, 329.0, 350.0)))));
    g_season.foliage = foliage;
    g_season.phase = d < 135.0 ? 0 : (d < 262.0 ? 1 : 2);
    g_season.key = (int)(snow * 20.0) * 10000 + (int)(leaf * 20.0) * 100 + (int)(flowers * 20.0) +
                   (int)(foliage * 20.0) * 1000000 + g_season.phase * 100000000;  // when it changes, the ground and the trees are rebuilt
}

// Share of the stars that are out: all of them at night, none by day, fading in and out around dusk and dawn.
static double star_factor(const SkyClock *c) {
    double h = c->hour;
    if (h < c->rise - 0.75 || h > c->set + 0.75) return 1.0;
    if (h < c->rise + 0.25) return 1.0 - (h - (c->rise - 0.75));
    if (h > c->set - 0.25) return h - (c->set - 0.25);
    return 0.0;
}

typedef struct { int x, y; const char *s; int bold; } SkyLine;

static const char *const CLOUDS[3][3] = {
    {"    .--.    ", " .-(    ).  ", "(___.__)__) "},
    {"   .--.  .-.    ", " .(    )(   ).  ", "(__________.__) "},
    {"   .-.    ", " .(   ).  ", "(_.___)_) "},
};

// Collects the lines of everything that hangs in the sky: sun, moon, clouds.
// The sun and the moon cross the sky over SKY_ARC_W scene columns around the picture, the clouds drift over the whole world;
// rows are scene rows too (the sky sizes are those of the lowest screen), `ox`/`oy` only turn them into screen cells.
#define SKY_ARC_X0 (-13)
#define SKY_ARC_W 100
static int sky_sprites(long t_ms, int ox, int oy, double wind, const SkyClock *c, SkyLine *out) {
    int n = 0;
    int horizon = 11;

    if (c->hour >= c->rise && c->hour <= c->set) {  // sun: a solid disc with eight breathing rays
        static char sun[11][22];
        static const char *const core[3] = {" .@@@@@. ", "(@@@@@@@)", " `@@@@@' "};  // a solid disc: no pupil, so no eye
        // ray i: where it starts (dx, dy from the centre), the step per link, and its character
        static const struct { int dx, dy, sx, sy; char ch; } rays[8] = {
            {6, 0, 2, 0, '-'},   {-6, 0, -2, 0, '-'}, {0, -3, 0, -1, '|'},  {0, 3, 0, 1, '|'},
            {6, -2, 2, -1, '/'}, {-6, 2, -2, 1, '/'}, {-6, -2, -2, -1, '\\'}, {6, 2, 2, 1, '\\'},
        };
        // The rays get shorter on a low screen so that the whole sun stays above the central peaks at noon.
        int links = 1;  // the short rays of the lowest screen: the sun looks the same on every window
        int mid = 2 + links, rows = 2 * mid + 1;  // row of the disc centre, number of rows of the sprite
        for (int r = 0; r < 11; ++r) {
            memset(sun[r], ' ', 21);
            sun[r][21] = '\0';
        }
        for (int k = 0; k < 3; ++k) memcpy(&sun[mid - 1 + k][6], core[k], 9);
        // The rays breathe: each one slowly grows and fades back (a cycle of about 7 seconds, every ray in its own
        // phase). The growing tip is a dot that turns into the full ray character, so even the shortest sun on a low
        // screen (one link per ray) visibly breathes, and the sun never flickers.
        for (int i = 0; i < 8; ++i) {
            double y = tri((double)t_ms / 7000.0 + (double)i / 8.0);
            double w = 0.5 - 0.5 * (1.5 * y - 0.5 * y * y * y);  // smooth 0..1
            double reach = w * links;                             // how many links are lit
            for (int k = 0; k < links; ++k) {
                double frac = reach - k;
                if (k == 0 && frac < 0.25) frac = 0.25;           // the first link never disappears
                if (frac < 0.25) break;
                sun[mid + rays[i].dy + k * rays[i].sy][10 + rays[i].dx + k * rays[i].sx] = frac >= 0.7 ? rays[i].ch : '.';
            }
        }
        double u = (c->hour - c->rise) / (c->set - c->rise);
        int sun_top = mid;  // keeps the upper rays on the screen
        int sun_horizon = horizon > sun_top + 3 ? horizon : sun_top + 3;
        int cx = ox + SKY_ARC_X0 + (int)(u * SKY_ARC_W);
        int cy = oy + sun_horizon - (int)(4.0 * u * (1.0 - u) * (sun_horizon - sun_top) + 0.5);
        for (int r = 0; r < rows; ++r) out[n++] = (SkyLine){cx - 10, cy - mid + r, sun[r], 0};
    }

    // The moon: a full 11x5 disc with an outline and craters, drawn bold (the brightest a monochrome terminal can
    // make it). It is up for the whole night, rising at sunset and setting at sunrise, and stays high for most of it.
    static const char *const moon[5] = {"  .-\"\"\"-.  ", " /. o. O.\\ ", "|.o .O o. |", " \\o. o. O/ ", "  `-...-'  "};
    double night_len = 24.0 - (c->set - c->rise);
    double dn = wrap(c->hour - c->set, 24.0);
    if (dn <= night_len) {
        double u = dn / night_len;
        double a = 2.0 * u - 1.0;
        double alt = 1.0 - a * a * a * a;  // a flat arc: high for most of the night, down at the horizon at both ends
        int moon_horizon = 5, moon_top = 2;
        int cx = ox + SKY_ARC_X0 + (int)(u * SKY_ARC_W);
        int cy = oy + moon_horizon - (int)(alt * (moon_horizon - moon_top) + 0.5);
        for (int r = 0; r < 5; ++r) out[n++] = (SkyLine){cx - 5, cy - 2 + r, moon[r], 1};
    }

    // A gust only speeds the clouds up (smoothly, along the wind): the extra distance is the integral of the gust.
    (void)wind;
    double cloud_boost = 2.5 * g_wind_push;

    int clouds = 5;               // over the whole world: about two on a window of the usual width
    int span = WORLD_W + 48;
    int avail = 14;               // scene rows -8..5: the clouds above the picture show on tall windows only
    for (int i = 0; i < clouds; ++i) {
        const char *const *shape = CLOUDS[hash2(i, 0, 81) % 3];
        double speed = 0.4 + (double)(hash2(i, 0, 82) % 35) / 100.0;  // cells per second
        double travel = speed * (double)t_ms / 1000.0 + cloud_boost;
        double x = wrap((double)(hash2(i, 0, 83) % (unsigned)span) + g_wind_dir * travel, (double)span);
        int cx = ox + WORLD_X0 + (int)x - 24;
        int cy = oy - 8 + (i * avail) / clouds + (int)(hash2(i, 0, 84) % 2);
        for (int r = 0; r < 3; ++r) out[n++] = (SkyLine){cx, cy + r, shape[r], 0};
    }
    return n;
}

// Paints one sprite line on sky cells only (mountains and trees stay in front). The span between the first and the
// last visible character is opaque, so a cloud also hides the stars behind it.
static void paint_sky_line(char *row, unsigned char *bold, int y, const SkyLine *l) {
    int first = 0, last = (int)strlen(l->s) - 1;
    while (first <= last && l->s[first] == ' ') ++first;
    while (last >= first && l->s[last] == ' ') --last;
    for (int j = first; j <= last; ++j) {
        int x = l->x + j;
        if (x >= 0 && x < scr_cols && skym[(size_t)y * scr_cols + x]) {
            row[x] = l->s[j];
            bold[x] = (unsigned char)l->bold;
        }
    }
}

// Snow comes in showers: the time is cut into windows of 3 minutes, and a window has a shower of 1.5-2.5 minutes with
// a probability that depends on the time of day: 15% of the windows by day (about 10% of the time), 90% at night (about
// 60% of the time), smoothly in between at dusk and dawn. A shower fades in and out over 20 seconds. The result is
// the strength of the snowfall right now, 0..1.
static double snow_episode(long t_ms) {
    if (g_demo) return 1.0;
    const long window = 180000;
    long te = t_ms + g_ev_off, k = te / window;
    SkyClock c0;
    sky_clock(k * window - g_ev_off, &c0);  // the real time of the window start
    double p = 0.15 + 0.75 * star_factor(&c0);
    if ((double)(hash2((int)k, 0, 141) % 1000) / 1000.0 >= p) return 0.0;
    long len = 90000 + (long)(hash2((int)k, 0, 142) % 60000);
    long start = k * window + (long)(hash2((int)k, 0, 143) % (unsigned)(window - len));
    long dt = te - start;
    if (dt < 0 || dt >= len) return 0.0;
    double edge = (double)(dt < len - dt ? dt : len - dt) / 20000.0;
    return edge >= 1.0 ? 1.0 : edge * edge * (3.0 - 2.0 * edge);
}

// A thing that moves through the picture and is painted on top: a snowflake, a leaf, a bird, a butterfly wing.
// `soft` things may also be painted over meadow and leaf cells, the others only over empty air.
typedef struct { int x, y; char ch; int soft; } Flake;
#define MAX_FLAKES 1500

static int soft_cell(char c) {
    return c == ' ' || c == ',' || c == '"' || c == ';' || c == '\'' || c == '.' || c == ':' || c == '~';
}

// Everything that falls, flies or flutters, as a function of the time alone:
//  - snow in winter falls from the sky, drifts along the wind and is carried further by gusts;
//  - in autumn the leaves leave the crowns of the deciduous trees, fall to the ground (the wind pushes them while
//    they fall) and rest there a while before the next one comes off;
//  - in autumn a flock of birds now and then crosses the sky in a wedge, with the wind;
//  - in spring and summer by day butterflies wander over the meadow.
static int weather(long t_ms, int ox, int oy, Flake *out) {
    int n = 0;
    double t = (double)t_ms / 1000.0;
    int total = WORLD_W * (SKY_TOP + 18) / 17;  // as dense as before, over the whole world
    if (total > MAX_FLAKES) total = MAX_FLAKES;

    double snow_d = g_season.snow > 0.01 ? g_season.snow * (0.5 + 0.5 * (0.5 + 0.5 * tri(t / 90.0))) * snow_episode(t_ms) : 0.0;
    if (snow_d >= 0.01) {
        for (int i = 0; i < total && n < MAX_FLAKES; ++i) {
            if (hash2(i, 0, 104) % 1000 >= (unsigned)(snow_d * 1000.0)) continue;
            unsigned hx = hash2(i, 0, 101), hy = hash2(i, 0, 102), hs = hash2(i, 0, 103);
            int span = SKY_TOP + 12 + (int)(hs % 12);  // from SKY_TOP rows above the picture down to the row where this flake lands
            double phase = (double)(hx % 1000) / 1000.0;
            double speed = 0.45 * (0.9 + (double)((hs >> 8) % 100) / 100.0);  // rows per second: snow falls slowly
            double sway = 1.6 * tri(t / (3.0 + (double)((hs >> 16) % 4)) + phase);
            double drift = g_wind_dir * (0.4 * t + 4.0 * g_wind_push);
            unsigned q = (hs >> 20) % 100;
            int y = (int)wrap((double)(hy % (unsigned)span) + speed * t, (double)span) - SKY_TOP + oy;
            int x = (int)wrap((double)(hx % (unsigned)WORLD_W) + drift + sway, (double)WORLD_W) + WORLD_X0 + ox;
            if (x < 0 || x >= scr_cols || y < 0 || y >= scr_rows) continue;
            out[n++] = (Flake){x, y, q < 55 ? '.' : (q < 85 ? '*' : '\''), 0};
        }
    }

    if (g_season.leaf > 0.02 && g_ncrowns > 0) {  // leaves coming off the crowns
        // Calm weather: a few leaves come off now and then (30% of the old number). When a gust begins, an extra
        // batch (150% of the old number) is torn off the crowns during its first part and carried on by the wind.
        long te = t_ms + g_ev_off;
        int calm = (int)(g_ncrowns * 6.0 * g_season.leaf * 0.30);
        int gusty = (int)(g_ncrowns * 6.0 * g_season.leaf * 1.5);
        if (calm > 300) calm = 300;
        if (gusty > 1000) gusty = 1000;
        int kw = (int)(te / WIND_WINDOW_MS);
        for (int pass = 0; pass < 4; ++pass) {  // pass 0: the calm leaves; 1..3: the gusts of this and the two earlier windows
            long gstart = 0, gdur = 0;
            int count = calm, salt = 0;
            if (pass > 0) {
                int kk = kw - (pass - 1);
                if (kk < 0 || !wind_gust(kk, &gstart, &gdur)) continue;
                count = gusty;
                salt = 1 + kk;
            }
            for (int i = 0; i < count && n < MAX_FLAKES; ++i) {
                unsigned h1 = hash2(i, salt, 111), h2 = hash2(i, salt, 112), h3 = hash2(i, salt, 113);
                const Crown *c = &g_crowns[h1 % (unsigned)g_ncrowns];
                int row = (int)(h2 % (unsigned)c->ch);
                int hw = crown_half(c->ch, row);
                double x0 = (double)(c->cx + ox + (int)(h3 % (unsigned)(2 * hw + 1)) - hw);
                double y0 = (double)(c->top + row + oy);
                int ground_row = c->base + 2 + (int)((h2 >> 8) % 4);
                int cap = c->base <= 23 ? 23 : 1000;  // an oak of the meadow drops its leaves on the meadow, not in the river
                double ground = (double)((ground_row > cap ? cap : ground_row) + oy);
                double speed = 0.7 + (double)((h3 >> 8) % 100) / 170.0;                // rows per second
                double fall = (ground - y0) / speed;                                    // seconds in the air
                double age;
                if (pass == 0) {
                    double period = fall + 1.0 + (double)((h1 >> 8) % 70) / 10.0;       // then it waits for the next leaf
                    age = wrap((double)(h1 >> 16) / 65536.0 * period + t, period);
                } else {
                    long delay = (long)(hash2(i, salt, 114) % (unsigned)(gdur * 8 / 10));  // torn off during the first part of the gust
                    age = (double)(te - (gstart + delay)) / 1000.0;
                }
                if (age < 0.0 || age >= fall || age > 24.0) continue;
                double sway = 3.0 * tri(age / (3.0 + (double)(h3 % 4)) + (double)(h2 % 1000) / 1000.0);
                double push = wind_push_at(t) - wind_push_at(t - age);                  // the wind it has met on the way
                double x = x0 + sway + g_wind_dir * (0.5 * age + 7.0 * push);
                unsigned q = (h3 >> 20) % 100;
                out[n++] = (Flake){(int)x, (int)(y0 + speed * age), q < 60 ? '~' : (q < 85 ? ',' : '\''), 1};
            }
        }
    }

    if (g_season.leaf > 0.15 && g_star_f < 0.4) {  // a flock of birds in a wedge, flying with the wind
        long window = 80000, k = t_ms / window;
        if (g_demo || hash2((int)k, 0, 121) % 100 < 55) {
            long start = k * window + 4000 + (long)(hash2((int)k, 0, 122) % 30000);
            long dt = t_ms - start;
            if (dt >= 0 && dt < 30000) {
                double p = (double)dt / 30000.0;
                int dir = g_wind_dir;
                int lead_x = ox + (dir > 0 ? (int)(-77.0 + p * 228.0) : (int)(151.0 - p * 228.0));  // across 200 scene columns
                int lead_y = oy + (int)(hash2((int)k, 0, 123) % 9);
                for (int b = 0; b < 7 && n < MAX_FLAKES; ++b) {
                    int rank = (b + 1) / 2;
                    int by = b == 0 ? lead_y : lead_y + (b % 2 ? rank : -rank);
                    if (by < 0) continue;
                    out[n++] = (Flake){lead_x - dir * 3 * rank, by, ((t_ms / 450) + b) % 2 ? 'v' : '^', 0};
                }
            }
        }
    }

    if (g_star_f < 0.2 && g_season.snow < 0.05 && g_season.flowers > 0.25) {  // butterflies by day
        int butterflies = (int)((double)WORLD_W / 26.0 * g_season.flowers + 0.5);
        if (butterflies < 1) butterflies = 1;
        if (butterflies > 24) butterflies = 24;
        for (int b = 0; b < butterflies && n + 1 < MAX_FLAKES; ++b) {
            unsigned h1 = hash2(b, 0, 131), h2 = hash2(b, 0, 132);
            double ph = (double)((h1 >> 10) % 1000) / 1000.0;
            double x = (double)(ox + WORLD_X0 + (int)(h1 % (unsigned)WORLD_W)) + 6.0 * tri(t / 7.0 + ph) + 2.0 * tri(t / 2.6 + ph * 3.0);
            int on_bank = b % 3 == 2;  // every third butterfly flies over the lower bank (scene rows 32..37)
            double y0 = on_bank ? (double)(oy + 32 + (int)(h2 % 6)) : (double)(oy + 14 + (int)(h2 % 9));
            double y = y0 + 1.6 * tri(t / 5.0 + ph * 2.0) + 0.6 * tri(t / 1.7 + ph);
            int open = ((long)(t * 4.0) + b) % 2;
            if (x < -2 || x >= scr_cols || y < 0 || y >= scr_rows) continue;
            out[n++] = (Flake){(int)x, (int)y, open ? '}' : ')', 1};
            out[n++] = (Flake){(int)x + 1, (int)y, open ? '{' : '(', 1};
        }
    }
    return n;
}

// Whether the river cell is frozen (and stands still). The ice spreads in patches, starting from the banks: the two rows
// next to the shores freeze first, the middle ones later; when the river is frozen over every cell is.
static int river_frozen(int sx, int sy) {
    double p = g_season.ice * ((sy == RIVER_ROW_FIRST || sy == RIVER_ROW_LAST) ? 1.6 : 1.0);
    double u = 0.65 * (double)(hash2(floor_div(sx, 5), sy, 98) % 1000) / 1000.0 + 0.35 * (double)(hash2(sx, sy, 96) % 1000) / 1000.0;
    return u < p;
}

// Paints the whole screen: static background, twinkling stars, smoke and flowing river.
static void draw_scene(long t_ms) {
    SkyClock clock;
    sky_clock(t_ms, &clock);
    season_update(&clock);
    if (bg_rows != scr_rows || bg_cols != scr_cols || bg_key != g_season.key) {
        build_background();
    }
    int ox = (scr_cols - PIC_W) / 2, oy = (scr_rows - PIC_H) / 2;
    int smoke_alt = (int)((t_ms / (wabs(wind_at(t_ms)) > 0.05 ? 550 : 1100)) % 2);  // the smoke alternates twice as fast in the wind
    long river_step = t_ms / 350;
    int river_size = RIVER_ROW_LAST - RIVER_ROW_FIRST + 1;
    char *row = malloc((size_t)scr_cols);
    unsigned char *bold = malloc((size_t)scr_cols);
    if (row == NULL || bold == NULL) {
        exit(1);
    }
    double wind = wind_at(t_ms);
    wind_push_update(t_ms, wind);
    g_star_f = star_factor(&clock);
    Flake flakes[MAX_FLAKES];
    int n_flakes = weather(t_ms, ox, oy, flakes);
    SkyLine sky_lines[40];
    int n_sky = sky_sprites(t_ms, ox, oy, wind, &clock, sky_lines);
    Fish fish = fish_at(t_ms);
    fish.x0 += ox;  // the plan is in scene columns
    CatPose cat = cat_pose(t_ms);
    SheepPose sheep = sheep_pose(t_ms);
    Sprite sprites[MAX_SPRITES];
    int n_sprites = fish_sprites(&fish, oy, sprites);

    for (int y = 0; y < scr_rows; ++y) {
        int sy = y - oy;
        memset(bold, 0, (size_t)scr_cols);
        if (sy >= RIVER_ROW_FIRST && sy <= RIVER_ROW_LAST) {
            int i = sy - RIVER_ROW_FIRST;
            long shift = river_step * (min(i, river_size - i - 1) + 1);
            for (int x = 0; x < scr_cols; ++x) {
                row[x] = river_char(sy, (long)(x - ox) + shift + 1000000);
                if (g_season.ice > 0.0 && river_frozen(x - ox, sy)) {
                    unsigned q = hash2(x - ox, sy, 97) % 100;  // frozen: this cell stands still and turns into ice
                    row[x] = q < 70 ? '=' : (q < 92 ? '-' : '.');
                }
            }
        } else {
            for (int x = 0; x < scr_cols; ++x) {
                size_t i = (size_t)y * scr_cols + x;
                int sx = x - ox;
                char c = bg[i];
                if (smoke_alt && sy >= SMOKE_BEGIN && sy <= SMOKE_END && is_art(sx, sy)) {
                    c = lines[SMOKE_BEGIN1 + sy - SMOKE_BEGIN][sx];
                    if (c == ' ' && skym[i]) c = star_char(sx, sy, t_ms);
                } else if (skym[i]) {
                    c = star_char(sx, sy, t_ms);
                }
                row[x] = c;
            }
            if (wabs(wind) > 0.05) {
                apply_wind(row, y, sy, ox, wind, t_ms);
            }
            for (int k = 0; k < n_sky; ++k) {
                if (sky_lines[k].y == y) paint_sky_line(row, bold, y, &sky_lines[k]);
            }
            for (int k = 0; k < n_flakes; ++k) {  // snow and birds only through empty air; leaves and butterflies over the meadow too
                const Flake *f = &flakes[k];
                if (f->y == y && f->x >= 0 && f->x < scr_cols &&
                    (row[f->x] == ' ' || (f->soft && soft_cell(row[f->x]) && !is_art(f->x - ox, sy)))) {  // never over the drawn picture
                    row[f->x] = f->ch;
                }
            }
        }
        draw_sheep(row, y, ox, oy, &sheep);
        draw_cat(row, y, ox, oy, t_ms, &cat, g_season.ice >= 0.5);
        for (int k = 0; k < n_sprites; ++k) {
            const Sprite *sp = &sprites[k];
            int len = (int)strlen(sp->s);
            int dy = y - sp->y;
            if (sp->knock && (dy == 0 || (sp->knock == 2 && (dy == 1 || dy == -1)))) {
                for (int x = sp->x - 1; x <= sp->x + len; ++x) {
                    if (x >= 0 && x < scr_cols) row[x] = ' ';
                }
            }
            if (dy == 0) {
                for (int j = 0; j < len; ++j) {
                    int x = sp->x + j;
                    if (x >= 0 && x < scr_cols) row[x] = sp->s[j];
                }
            }
        }
        fb_move(y, 0);
        for (int x = 0; x < scr_cols;) {  // write the row in runs of equal weight
            int x2 = x;
            while (x2 < scr_cols && bold[x2] == bold[x]) ++x2;
            if (bold[x]) fb_str("\033[1m");
            fb_put(row + x, (size_t)(x2 - x));
            if (bold[x]) fb_str("\033[22m");
            x = x2;
        }
    }
    free(row);
    free(bold);
}

// ---------------------------------------------------

// Function to read the file and store its lines
int read_file_lines(const char *landscape_file, char lines[][LINE_LENGTH], int max_lines) {
    FILE *file = fopen(landscape_file, "r");
    if (file == NULL) {
        perror("Error opening file");
        exit(1);
    }

    int line_count = 0;
    while (fgets(lines[line_count], LINE_LENGTH, file) != NULL) {
        // Remove newline character at the end of each line
        size_t len = strlen(lines[line_count]);
        if (len > 0 && lines[line_count][len - 1] == '\n') {
            lines[line_count][len - 1] = '\0';
        }

        ++line_count;
        if (line_count >= max_lines) {
            break;
        }
    }

    fclose(file);
    return line_count;
}

// Rows are drawn as fixed-width blocks: turn the NUL padding of short lines into spaces.
static void pad_picture(int total) {
    for (int i = 0; i < total; ++i) {
        size_t len = strlen(lines[i]);
        line_len[i] = (int)len;
        for (size_t j = len; j < LINE_LENGTH; ++j) {
            lines[i][j] = ' ';
        }
    }
}

// Blocks until the terminal is big enough for the picture (or a stop was requested).
static void wait_for_size() {
    while (!stop_looping) {
        sync_size();
        if (size_ok()) {
            return;
        }
        draw_notice();
        fb_flush();
        usleep(100000);
        drain_input();
    }
}

static void render_message(int total_lines) {
    for (int i = 0; i < total_lines && !stop_looping; ++i) {
        int len = (int)strlen(message[i]);
        for (int n = 1; n <= len && !stop_looping; ++n) {
            sync_size();
            draw_centered(message[i], n, scr_rows / 2);
            fb_flush();
            sleep_ms(40);
        }
        sleep_ms(800);
        sync_size();
        draw_centered("", 0, scr_rows / 2);
        fb_flush();
        sleep_ms(70);
    }
}

// Like sleep_ms, but keeps sleeping through the farewell and only a repeated Ctrl+C cuts it short.
static void bye_sleep_ms(int ms) {
    while (ms > 0 && !fast_exit) {
        int step = min(ms, 10);
        usleep(step * 1000);
        ms -= step;
    }
}

static void render_bye() {
    const char bye[] = "Good luck on your journey, wanderer...";
    int len = (int)strlen(bye);
    int first_dot = (int)(strchr(bye, '.') - bye);
    fb_str("\033[2J");
    for (int n = 1; n <= len && !fast_exit; ++n) {
        sync_size();
        draw_centered(bye, n, scr_rows / 2);
        fb_flush();
        bye_sleep_ms(n > first_dot ? 250 : 35);
    }
    bye_sleep_ms(400);
}

// Runs until Ctrl+C / SIGTERM / SIGHUP. Repaints whenever something changed and at least once a second,
// so a corrupted or forgotten screen heals by itself.
static void run_scene() {
    long t0 = now_ms();
    long last_sig = -1, last_paint = -1000000;
    while (!stop_looping) {
        long t = now_ms() - t0;
        int changed = sync_size();
        SkyClock season_clock;
        sky_clock(t, &season_clock);
        season_update(&season_clock);
        long sig = (t / 350) * 2 + (t / 1100) % 2;
        if (fish_at(t).active || wabs(wind_at(t)) > 0.05 || cat_blinks(t) || cat_pose(t).moving || sheep_pose(t).present) {
            sig += (t / 80) * 1000003L;  // repaint often while a fish is in the air or the wind blows
        } else if (g_season.snow > 0.02 || g_season.leaf > 0.02 || g_season.flowers > 0.25) {
            sig += (t / 160) * 1000003L;  // and fairly often while snow or leaves are falling
        }
        if (changed || sig != last_sig || t - last_paint >= 1000) {
            if (size_ok()) {
                draw_scene(t);
            } else {
                draw_notice();
            }
            fb_flush();
            last_sig = sig;
            last_paint = t;
        }
        usleep(30000);
        drain_input();
    }
}

// Async-signal-safe: only raises flags. Everything else happens in main().
void handle_signal(int signal) {
    if (signal == SIGHUP) {
        hangup = 1;  // the terminal is gone, nobody to say goodbye to
        stop_looping = 1;
    } else if (signal == SIGWINCH || signal == SIGCONT) {
        need_redraw = 1;
    } else {
        if (++interrupts >= 2) {
            fast_exit = 1;  // pressed again while the farewell is playing: leave right now
        }
        stop_looping = 1;
    }
}

int main(int argc, char *argv[]) {
    if (argc == 2 && (strcmp(argv[1], "--help") == 0 || strcmp(argv[1], "-h") == 0)) {
        print_help(argv[0]);
        return 0;
    }
    if (argc == 2 && (strcmp(argv[1], "--version") == 0 || strcmp(argv[1], "-V") == 0)) {
        printf("%s\n", LODGE_VERSION);
        return 0;
    }
    if (argc != 3) {
        print_help(argv[0]);
        return 1;
    }

    disable_input();
    atexit(cleanup);

    signal(SIGINT, handle_signal);
    signal(SIGTERM, handle_signal);
    signal(SIGHUP, handle_signal);
    signal(SIGWINCH, handle_signal);
    signal(SIGCONT, handle_signal);
    signal(SIGTSTP, SIG_IGN);
    signal(SIGQUIT, SIG_IGN);
#ifdef SIGINFO
    signal(SIGINFO, SIG_IGN);
#endif

    landscape_file = argv[1];
    message_file = argv[2];

    int total_land_lines = read_file_lines(landscape_file, lines, LINES);
    int total_message_lines = read_file_lines(message_file, message, LINES);

    if (total_land_lines == 0) {
        printf("The file is empty or could not be read.\n");
        return 1;
    }
    pad_picture(LINES);
    sky_init();
    wind_init();

    enter_screen();
    if (getenv("LODGE_WAIT_RESIZE") != NULL) {
        wait_for_stable_size();  // only needed when a launcher resizes the window after start
    }

    wait_for_size();
    if (!stop_looping) {
        render_message(total_message_lines);
    }
    if (!stop_looping) {
        run_scene();
    }

    if (!hangup) {
        render_bye();
    }
    free(fb);
    return 0;  // atexit(cleanup) restores the screen and terminal settings
}
