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

static int line_len[LINES];       // real length of every picture row (the rest is padding)
static char *bg = NULL;           // static background, one cell per screen cell
static unsigned char *skym = NULL; // 1 = sky cell (stars may appear here)
static int bg_rows = 0, bg_cols = 0;

static unsigned hash2(int x, int y, unsigned salt) {
    unsigned h = (unsigned)x * 374761393u + (unsigned)y * 668265263u + salt * 2246822519u;
    h = (h ^ (h >> 13)) * 1274126177u;
    return h ^ (h >> 16);
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
    if (sy >= PIC_H) {  // near bank in front of the river
        unsigned h = hash2(sx, sy, 11) % 100;
        *ch = h < 55 ? ',' : (h < 65 ? '\'' : (h < 72 ? '.' : (h < 74 ? '@' : ' ')));
    } else if (sy == SHORE_UP || sy == SHORE_DOWN) {
        *ch = '`';
    } else if (sy >= RIVER_ROW_FIRST && sy <= RIVER_ROW_LAST) {
        *ch = ' ';  // painted per frame
    } else if (sy >= ground_top(sx)) {
        if ((sy == 20 || sy == 21) && sx >= PIC_W - 2) {
            *ch = '_';  // the bank line of the picture goes on to the right
        } else {
            *ch = hash2(sx, sy, 1) % 100 < 2 ? '@' : ',';
        }
    } else {
        *sky = 1;
    }
}

// Puts a generated (non-picture) cell on the screen.
static void put_ext(int ox, int oy, int sx, int sy, char ch, int solid) {
    int x = sx + ox, y = sy + oy;
    if (x < 0 || x >= scr_cols || y < 0 || y >= scr_rows || is_art(sx, sy)) return;
    bg[(size_t)y * scr_cols + x] = ch;
    if (solid) skym[(size_t)y * scr_cols + x] = 0;
}

static void draw_mountain(int ox, int oy, int cx, int h, int base) {
    int top = base - h;
    for (int r = top; r <= base; ++r) {
        int d = r - top, l = cx - d, rr = cx + 1 + d;
        for (int x = l + 1; x < rr; ++x) {
            put_ext(ox, oy, x, r, hash2(x, r, 7) % 100 < 30 ? '^' : ' ', 1);
        }
        put_ext(ox, oy, l, r, '/', 1);
        put_ext(ox, oy, rr, r, '\\', 1);
    }
}

static void draw_tree(int ox, int oy, int cx, int h, int base) {
    for (int i = 0; i < h; ++i) {
        int r = base - h + i, l = cx - 1 - i, rr = cx + 1 + i;
        for (int x = l + 1; x < rr; ++x) {
            put_ext(ox, oy, x, r, (i >= 1 && hash2(x, r, 9) % 100 < 20) ? 'o' : '|', 1);
        }
        put_ext(ox, oy, l, r, '/', 1);
        put_ext(ox, oy, rr, r, '\\', 1);
    }
    put_ext(ox, oy, cx - h, base, '\'', 1);
    put_ext(ox, oy, cx - 1, base, '|', 1);
    put_ext(ox, oy, cx + 1, base, '|', 1);
    put_ext(ox, oy, cx + h, base, '\'', 1);
}

// Chains of mountains / rows of trees marching outwards from the picture. side = -1 (left) or +1 (right).
static void build_side(int ox, int oy, int side, int extent) {
    int cxs[MAX_PEAKS], hs[MAX_PEAKS], n = 0;
    int edge = side < 0 ? -3 : PIC_W + 2;
    for (int k = 0; k < MAX_PEAKS && (side < 0 ? edge > -extent - 24 : edge < PIC_W + extent + 24); ++k) {
        int h = 5 + (int)(hash2(k, side, 21) % 8);
        int gap = (int)(hash2(k, side, 22) % 4) - 1;
        int cx = side < 0 ? edge - gap - h : edge + gap + h;
        cxs[n] = cx;
        hs[n++] = h;
        edge = side < 0 ? cx - h : cx + 1 + h;
    }
    for (int k = n - 1; k >= 0; --k) {  // farthest first, the nearest ones stand in front
        draw_mountain(ox, oy, cxs[k], hs[k], mount_base(side < 0 ? -1 : PIC_W));
    }

    edge = side < 0 ? -3 : PIC_W + 2;
    for (int k = 0; k < MAX_PEAKS && (side < 0 ? edge > -extent - 24 : edge < PIC_W + extent + 24); ++k) {
        int h = 4 + (int)(hash2(k, side, 31) % 4);
        int base = 16 + (int)(hash2(k, side, 32) % 3);
        int cx = side < 0 ? edge - h - 1 : edge + h + 1;
        draw_tree(ox, oy, cx, h, base);
        int space = 2 + (int)(hash2(k, side, 33) % 7);
        edge = side < 0 ? cx - h - 1 - space : cx + h + 1 + space;
    }
}

static void build_background() {
    size_t n = (size_t)scr_rows * (size_t)scr_cols;
    bg = realloc(bg, n);
    skym = realloc(skym, n);
    if (bg == NULL || skym == NULL) {
        exit(1);
    }
    bg_rows = scr_rows;
    bg_cols = scr_cols;
    int ox = (scr_cols - PIC_W) / 2, oy = (scr_rows - PIC_H) / 2;

    for (int y = 0; y < scr_rows; ++y) {
        for (int x = 0; x < scr_cols; ++x) {
            size_t i = (size_t)y * scr_cols + x;
            int sx = x - ox, sy = y - oy;
            if (is_art(sx, sy)) {
                bg[i] = lines[sy][sx];
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

static Fish fish_at(long t_ms) {
    Fish f = {0, 0, 0, 0, 3, 1};
    int k = (int)(t_ms / FISH_WINDOW_MS);
    if (!g_demo && hash2(k, 0, 51) % 100 >= 15) {
        return f;  // only 15 windows out of 100 have a jump: about one a minute
    }
    long start = (long)k * FISH_WINDOW_MS + 1500 + (long)(hash2(k, 0, 52) % 5500);
    long dt = t_ms - start;
    if (dt < 0 || dt >= FISH_EVENT_MS) {
        return f;
    }
    f.active = 1;
    f.dt = dt;
    f.dir = hash2(k, 0, 55) % 2 ? 1 : -1;                   // left or right, equally likely
    f.row0 = RIVER_ROW_LAST - (int)(hash2(k, 0, 54) % 2);  // takes off from one of the two lower river rows

    // A tall jump needs open grass on one side of the picture, so the fish never crosses the hut and the trees.
    // `lo..hi` is where the jump may start counted as if it went right; a left jump starts FISH_DX further right.
    int ox = (scr_cols - PIC_W) / 2, room = FISH_DX + 12;
    int left_ok = ox >= room + 6, right_ok = scr_cols - ox - PIC_W >= room + 6;
    unsigned pick = hash2(k, 0, 53);
    int base;
    if (left_ok || right_ok) {
        int use_left = left_ok && (!right_ok || pick % 2 == 0);
        int region_start = use_left ? 3 : ox + PIC_W + 3;
        int region_end = use_left ? ox - 3 : scr_cols - 3;
        int span = region_end - region_start - FISH_DX - 8;
        base = region_start + 4 + (int)((pick / 2) % (unsigned)(span > 0 ? span : 1));
        f.h = 6;
    } else {
        base = 6 + (int)(pick % (unsigned)(scr_cols > 36 + FISH_DX ? scr_cols - 30 - FISH_DX : 6));
        f.h = 3;  // narrow screen: keep the low jump
    }
    f.x0 = f.dir > 0 ? base : base + FISH_DX;
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
    const char *d = getenv("LODGE_WIND_DIR");  // "left" or "right", for testing
    if (d != NULL && (d[0] == 'l' || d[0] == 'L')) g_wind_dir = -1;
    if (d != NULL && (d[0] == 'r' || d[0] == 'R')) g_wind_dir = 1;
}

static double wind_at(long t_ms) {
    int k = (int)(t_ms / WIND_WINDOW_MS);
    if (!g_demo && hash2(k, 0, 61) % 100 >= 30) {
        return 0.0;  // a calm window (3 windows out of 10 have a gust)
    }
    long start = (long)k * WIND_WINDOW_MS + 1500 + (long)(hash2(k, 0, 62) % 3000);
    long dur = 4000 + (long)(hash2(k, 0, 63) % 5000);
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

// The hand-drawn hut is never touched by the wind.
static int in_hut(int sx, int sy) {
    return sx >= 14 && sx <= 32 && sy >= 12 && sy <= 22;
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
}

static double wrap(double x, double m) {
    x -= (double)(long)(x / m) * m;
    return x < 0 ? x + m : x;
}

typedef struct { double hour, rise, set, phase; } SkyClock;

static void sky_clock(long t_ms, SkyClock *c) {
    double e = g_epoch0 + (double)t_ms / 1000.0 * g_scale;
    time_t tt = (time_t)e;
    struct tm tmv;
    localtime_r(&tt, &tmv);
    c->hour = tmv.tm_hour + tmv.tm_min / 60.0 + tmv.tm_sec / 3600.0;
    double y = tri(((double)tmv.tm_yday - 172.0) / 365.0);      // -1 at the June solstice, +1 in December
    double summer = -(1.5 * y - 0.5 * y * y * y);               // smoothed: +1 in June, -1 in December
    if (g_south) summer = -summer;
    double len = 12.0 + 4.0 * summer;                           // 8 h in midwinter, 16 h in midsummer
    c->rise = 12.5 - len / 2.0;
    c->set = 12.5 + len / 2.0;
    c->phase = wrap((e - 947182440.0) / 86400.0, 29.530588853) / 29.530588853;  // 0 = new moon, 0.5 = full
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
static int sky_sprites(long t_ms, int oy, double wind, const SkyClock *c, SkyLine *out) {
    int n = 0;
    int horizon = oy + 11, top = 2;
    if (horizon < top + 3) horizon = top + 3;

    if (c->hour >= c->rise && c->hour <= c->set) {  // sun: a solid disc with eight breathing rays
        static char sun[11][22];
        static const char *const core[3] = {" .@@@@@. ", "(@@@@@@@)", " `@@@@@' "};  // a solid disc: no pupil, so no eye
        // ray i: where it starts (dx, dy from the centre), the step per link, and its character
        static const struct { int dx, dy, sx, sy; char ch; } rays[8] = {
            {6, 0, 2, 0, '-'},   {-6, 0, -2, 0, '-'}, {0, -3, 0, -1, '|'},  {0, 3, 0, 1, '|'},
            {6, -2, 2, -1, '/'}, {-6, 2, -2, 1, '/'}, {-6, -2, -2, -1, '\\'}, {6, 2, 2, 1, '\\'},
        };
        // The rays get shorter on a low screen so that the whole sun stays above the central peaks at noon.
        int links = (oy - 4) / 2;
        links = links < 1 ? 1 : (links > 3 ? 3 : links);
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
        int cx = (int)(u * (scr_cols - 1));
        int cy = sun_horizon - (int)(4.0 * u * (1.0 - u) * (sun_horizon - sun_top) + 0.5);
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
        int moon_horizon = oy + 5, moon_top = 2;
        if (moon_horizon < moon_top + 3) moon_horizon = moon_top + 3;
        int cx = (int)(u * (scr_cols - 1));
        int cy = moon_horizon - (int)(alt * (moon_horizon - moon_top) + 0.5);
        for (int r = 0; r < 5; ++r) out[n++] = (SkyLine){cx - 5, cy - 2 + r, moon[r], 1};
    }

    // A gust only speeds the clouds up (smoothly, along the wind): the extra distance is the integral of the gust.
    static long last_t = 0;
    static double last_w = 0.0, cloud_boost = 0.0;
    if (t_ms > last_t) {
        cloud_boost += 2.5 * 0.5 * (wabs(last_w) + wabs(wind)) * (double)(t_ms - last_t) / 1000.0;
        last_t = t_ms;
        last_w = wind;
    }

    int clouds = 2 + (scr_cols >= 140) + (scr_cols >= 220);
    int span = scr_cols + 48;
    int avail = oy + 5 > 1 ? oy + 5 : 1;
    for (int i = 0; i < clouds; ++i) {
        const char *const *shape = CLOUDS[hash2(i, 0, 81) % 3];
        double speed = 0.4 + (double)(hash2(i, 0, 82) % 35) / 100.0;  // cells per second
        double travel = speed * (double)t_ms / 1000.0 + cloud_boost;
        double x = wrap((double)(hash2(i, 0, 83) % (unsigned)span) + g_wind_dir * travel, (double)span);
        int cx = (int)x - 24;
        int cy = 1 + (i * avail) / clouds + (int)(hash2(i, 0, 84) % 2);
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

// Paints the whole screen: static background, twinkling stars, smoke and flowing river.
static void draw_scene(long t_ms) {
    if (bg_rows != scr_rows || bg_cols != scr_cols) {
        build_background();
    }
    int ox = (scr_cols - PIC_W) / 2, oy = (scr_rows - PIC_H) / 2;
    int smoke_alt = (int)((t_ms / 1100) % 2);
    long river_step = t_ms / 350;
    int river_size = RIVER_ROW_LAST - RIVER_ROW_FIRST + 1;
    char *row = malloc((size_t)scr_cols);
    unsigned char *bold = malloc((size_t)scr_cols);
    if (row == NULL || bold == NULL) {
        exit(1);
    }
    double wind = wind_at(t_ms);
    SkyClock clock;
    sky_clock(t_ms, &clock);
    g_star_f = star_factor(&clock);
    SkyLine sky_lines[40];
    int n_sky = sky_sprites(t_ms, oy, wind, &clock, sky_lines);
    Fish fish = fish_at(t_ms);
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
        }
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
        long sig = (t / 350) * 2 + (t / 1100) % 2;
        if (fish_at(t).active || wabs(wind_at(t)) > 0.05) {
            sig += (t / 80) * 1000003L;  // repaint often while a fish is in the air or the wind blows
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
