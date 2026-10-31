/*
 * peafb - Peaclock-like Femboys: boykisser ASCII art in your terminal.
*/

#define _POSIX_C_SOURCE 200809L

#include <errno.h>
#include <math.h>
#include <signal.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ioctl.h>
#include <sys/select.h>
#include <termios.h>
#include <time.h>
#include <unistd.h>

#define VERSION "0.1"

struct pf_scheme {
    const char *name;
    int w, h;
    const uint8_t *rle;
    size_t rle_len;
};

struct pf_glyph {
    char ch;
    uint8_t dense;
    uint8_t z[8];
};

extern const struct pf_scheme pf_schemes[];
extern const int pf_nschemes;
extern const struct pf_glyph pf_glyphs[];
extern const int pf_nglyphs;

enum {
    C_OUT, C_WHITE, C_LINE, C_BLOB, C_RED, C_RLINE, C_PINK, C_LILAC, C_PAD,
    NCLASS
};

#define COL_BLACK 16
#define COL_WHITE 231
#define COL_RED   160
#define COL_RLINE 196
#define COL_PINK  224
#define COL_LILAC 225
#define COL_PAD   183
#define COL_DEFAULT (-1)

#define ZONES_X 2
#define ZONES_Y 4
#define NZONES (ZONES_X * ZONES_Y)
#define EXPAND 0.10         
#define THRESH_INK 0.20
#define THRESH_RED 0.12
#define BLOB_SOLID 0.33
#define INSIDE_BIAS 0.30
#define MAX_ROWS 300
#define MAX_COLS 600
#define MIN_ART_ROWS 8
#define MIN_ART_COLS 14
#define DEFAULT_ASPECT 2.0

struct cell {
    char ch;
    short fg;
    short bg;
};

static uint32_t *sat[NCLASS];
static int sat_scheme = -1;

static volatile sig_atomic_t g_quit = 0;
static volatile sig_atomic_t g_resize = 0;

static struct termios g_orig_termios;
static int g_have_termios = 0;
static int g_screen_active = 0;

static double g_aspect_override = 0.0;
static int g_show_hint = 1;

static int write_all(int fd, const char *p, size_t n)
{
    while (n > 0) {
        ssize_t w = write(fd, p, n);
        if (w < 0) {
            if (errno == EINTR || errno == EAGAIN)
                continue;
            return -1;
        }
        p += (size_t)w;
        n -= (size_t)w;
    }
    return 0;
}

struct buf {
    char *p;
    size_t n, cap;
    int failed;
};

static void buf_add(struct buf *b, const char *s, size_t n)
{
    if (b->failed)
        return;
    if (b->n + n + 1 > b->cap) {
        size_t ncap = b->cap ? b->cap : 4096;
        char *np;
        while (ncap < b->n + n + 1) {
            if (ncap > ((size_t)-1) / 2) {
                b->failed = 1;
                return;
            }
            ncap *= 2;
        }
        np = realloc(b->p, ncap);
        if (!np) {
            b->failed = 1;
            return;
        }
        b->p = np;
        b->cap = ncap;
    }
    memcpy(b->p + b->n, s, n);
    b->n += n;
    b->p[b->n] = '\0';
}

static void buf_str(struct buf *b, const char *s)
{
    buf_add(b, s, strlen(s));
}

static void buf_fmt(struct buf *b, const char *fmt, int a, int c)
{
    char tmp[48];
    int n = snprintf(tmp, sizeof tmp, fmt, a, c);
    if (n > 0 && (size_t)n < sizeof tmp)
        buf_add(b, tmp, (size_t)n);
}

static double clampd(double v, double lo, double hi)
{
    if (!(v >= lo))
        return lo;
    return v > hi ? hi : v;
}

static void free_sat(void)
{
    for (int k = 0; k < NCLASS; k++) {
        free(sat[k]);
        sat[k] = NULL;
    }
    sat_scheme = -1;
}

static int load_scheme(int idx)
{
    const struct pf_scheme *s;
    size_t total, stride, pos = 0, i = 0;
    uint8_t *px;

    if (idx < 0 || idx >= pf_nschemes)
        return -1;
    if (sat_scheme == idx)
        return 0;
    free_sat();

    s = &pf_schemes[idx];
    if (s->w <= 0 || s->h <= 0)
        return -1;
    total = (size_t)s->w * (size_t)s->h;
    stride = (size_t)s->w + 1;
    px = malloc(total);
    if (!px)
        return -1;

    while (i < s->rle_len) {
        uint8_t cls = s->rle[i++];
        uint64_t run = 0;
        int shift = 0;
        if (cls >= NCLASS)
            goto bad;
        for (;;) {
            uint8_t b;
            if (i >= s->rle_len)
                goto bad;
            b = s->rle[i++];
            run |= (uint64_t)(b & 0x7f) << shift;
            if (!(b & 0x80))
                break;
            shift += 7;
            if (shift > 28)
                goto bad;
        }
        if (run > total - pos)
            goto bad;
        memset(px + pos, cls, (size_t)run);
        pos += (size_t)run;
    }
    if (pos != total)
        goto bad;

    for (int k = 0; k < NCLASS; k++) {
        sat[k] = calloc(stride * ((size_t)s->h + 1), sizeof **sat);
        if (!sat[k])
            goto bad;
        for (int y = 0; y < s->h; y++) {
            uint32_t rowsum = 0;
            for (int x = 0; x < s->w; x++) {
                rowsum += (px[(size_t)y * (size_t)s->w + (size_t)x] == k);
                sat[k][(size_t)(y + 1) * stride + (size_t)(x + 1)] =
                    rowsum + sat[k][(size_t)y * stride + (size_t)(x + 1)];
            }
        }
    }
    free(px);
    sat_scheme = idx;
    return 0;

bad:
    free(px);
    free_sat();
    return -1;
}

static double box_mean(int k, double x0, double y0, double x1, double y1)
{
    const struct pf_scheme *s = &pf_schemes[sat_scheme];
    const size_t stride = (size_t)s->w + 1;
    const uint32_t *t = sat[k];
    int xi0, yi0, xi1, yi1;
    uint32_t v;

    x0 = clampd(x0, 0.0, s->w);
    x1 = clampd(x1, 0.0, s->w);
    y0 = clampd(y0, 0.0, s->h);
    y1 = clampd(y1, 0.0, s->h);

    xi0 = (int)floor(x0);
    yi0 = (int)floor(y0);
    xi1 = (int)ceil(x1);
    yi1 = (int)ceil(y1);
    if (xi1 < xi0 + 1) xi1 = xi0 + 1;
    if (yi1 < yi0 + 1) yi1 = yi0 + 1;
    if (xi1 > s->w) xi1 = s->w;
    if (yi1 > s->h) yi1 = s->h;
    if (xi0 > xi1 - 1) xi0 = xi1 - 1;
    if (yi0 > yi1 - 1) yi0 = yi1 - 1;

    v = t[(size_t)yi1 * stride + (size_t)xi1]
      - t[(size_t)yi0 * stride + (size_t)xi1]
      - t[(size_t)yi1 * stride + (size_t)xi0]
      + t[(size_t)yi0 * stride + (size_t)xi0];
    return (double)v / ((double)(xi1 - xi0) * (double)(yi1 - yi0));
}

static short class_color(int k)
{
    switch (k) {
    case C_WHITE: return COL_WHITE;
    case C_RED:   return COL_RED;
    case C_PINK:  return COL_PINK;
    case C_LILAC: return COL_LILAC;
    case C_PAD:   return COL_PAD;
    default:      return COL_DEFAULT;
    }
}

static int fit_art(const struct pf_scheme *s, int term_rows, int term_cols,
                   double aspect, int *out_r, int *out_c)
{
    const double ar = aspect * (double)s->w / (double)s->h;
    int mx = 0, my = 0, avail_r, avail_c, r, c;

    if (term_rows >= 12 && term_cols >= 24) {
        mx = 2;
        my = 1;
    }
    avail_r = term_rows - 2 * my;
    avail_c = term_cols - 2 * mx;
    if (avail_r > MAX_ROWS) avail_r = MAX_ROWS;
    if (avail_c > MAX_COLS) avail_c = MAX_COLS;
    if (avail_r < 1 || avail_c < 1)
        return 0;

    r = avail_r;
    c = (int)(r * ar + 0.5);
    if (c > avail_c) {
        r = (int)(avail_c / ar);
        c = (int)(r * ar + 0.5);
        if (c > avail_c)
            c = avail_c;
    }
    if (r < MIN_ART_ROWS || c < MIN_ART_COLS)
        return 0;
    *out_r = r;
    *out_c = c;
    return 1;
}

static void render_cells(struct cell *cells, int r, int c)
{
    const struct pf_scheme *s = &pf_schemes[sat_scheme];
    const double cw = (double)s->w / c;
    const double ch = (double)s->h / r;
    const double zw = cw / ZONES_X;
    const double zh = ch / ZONES_Y;
    const double gain = sqrt(zw * zh);

    for (int y = 0; y < r; y++) {
        for (int x = 0; x < c; x++) {
            struct cell *cl = &cells[(size_t)y * (size_t)c + (size_t)x];
            const double cx0 = x * cw, cx1 = (x + 1) * cw;
            const double cy0 = y * ch, cy1 = (y + 1) * ch;
            double f[NCLASS];
            double zi[NZONES], zr[NZONES];
            double imax = 0.0, rmax = 0.0;
            int feature = 0;            /* 0 none, 1 black, 2 red */
            int bgk;

            for (int k = 0; k < NCLASS; k++)
                f[k] = box_mean(k, cx0, cy0, cx1, cy1);

            cl->ch = ' ';
            cl->fg = COL_BLACK;
            cl->bg = COL_DEFAULT;

            if (f[C_BLOB] >= BLOB_SOLID) {
                cl->bg = COL_BLACK;
                continue;
            }

            for (int zy = 0; zy < ZONES_Y; zy++) {
                for (int zx = 0; zx < ZONES_X; zx++) {
                    double x0 = cx0 + zx * zw - EXPAND * cw;
                    double x1 = cx0 + (zx + 1) * zw + EXPAND * cw;
                    double y0 = cy0 + zy * zh - EXPAND * ch;
                    double y1 = cy0 + (zy + 1) * zh + EXPAND * ch;
                    double a = box_mean(C_LINE, x0, y0, x1, y1) * gain
                             + box_mean(C_BLOB, x0, y0, x1, y1);
                    double b = box_mean(C_RLINE, x0, y0, x1, y1) * gain * 1.8;
                    int k = zy * ZONES_X + zx;
                    zi[k] = a > 1.0 ? 1.0 : a;
                    zr[k] = b > 1.0 ? 1.0 : b;
                    if (zi[k] > imax) imax = zi[k];
                    if (zr[k] > rmax) rmax = zr[k];
                }
            }

            if (imax >= THRESH_INK && imax >= rmax * 0.9)
                feature = 1;
            else if (rmax >= THRESH_RED)
                feature = 2;

            if (feature) {
                const double *z = feature == 1 ? zi : zr;
                const int allow_dense = f[C_BLOB] > 0.05;
                double bestd = 1e30;
                int best = 0;
                for (int g = 0; g < pf_nglyphs; g++) {
                    double d = 0.0;
                    if (pf_glyphs[g].dense && !allow_dense)
                        continue;
                    for (int k = 0; k < NZONES; k++) {
                        double diff = pf_glyphs[g].z[k] / 255.0 - z[k];
                        d += diff * diff;
                    }
                    if (d < bestd) {
                        bestd = d;
                        best = g;
                    }
                }
                cl->ch = pf_glyphs[best].ch;
                cl->fg = feature == 1 ? COL_BLACK : COL_RLINE;
            }

            {
                static const int fills[] = { C_WHITE, C_PINK, C_LILAC, C_PAD, C_RED };
                double non_out = 0.0, best = -1.0;
                int bestk = C_WHITE;
                for (size_t i = 0; i < sizeof fills / sizeof fills[0]; i++) {
                    non_out += f[fills[i]];
                    if (f[fills[i]] > best) {
                        best = f[fills[i]];
                        bestk = fills[i];
                    }
                }
                if (f[C_BLOB] > 0.05 && non_out < 0.05) {
                    bgk = C_WHITE;
                } else if (feature) {
                    bgk = (non_out >= INSIDE_BIAS * (non_out + f[C_OUT])
                           && non_out > 0.0) ? bestk : C_OUT;
                } else {
                    bgk = (f[C_OUT] >= 0.5 || non_out <= 0.0) ? C_OUT : bestk;
                }
            }
            cl->bg = class_color(bgk);
            if (feature == 2 && cl->bg == COL_RED)
                cl->fg = COL_BLACK;
        }
    }
}

static char plain_char(const struct cell *cl)
{
    if (cl->ch != ' ')
        return cl->ch;
    if (cl->bg == COL_BLACK)
        return '@';
    if (cl->bg == COL_RED)
        return '%';
    return ' ';
}

static void emit_row(struct buf *b, const struct cell *line, int c,
                     int top_row, int left_col, int row)
{
    int first = 0, last = c - 1, cur_fg = -2, cur_bg = -2;

    while (first < c && line[first].ch == ' ' && line[first].bg == COL_DEFAULT)
        first++;
    if (first >= c)
        return;
    while (last > first && line[last].ch == ' ' && line[last].bg == COL_DEFAULT)
        last--;

    buf_fmt(b, "\x1b[%d;%dH", top_row + row + 1, left_col + first + 1);
    buf_str(b, "\x1b[0m");
    for (int x = first; x <= last; x++) {
        const struct cell *cl = &line[x];
        char tmp[24];
        int n;
        if (cl->bg != cur_bg) {
            if (cl->bg == COL_DEFAULT)
                n = snprintf(tmp, sizeof tmp, "\x1b[49m");
            else
                n = snprintf(tmp, sizeof tmp, "\x1b[48;5;%dm", cl->bg);
            if (n > 0 && (size_t)n < sizeof tmp)
                buf_add(b, tmp, (size_t)n);
            cur_bg = cl->bg;
        }
        if (cl->ch != ' ' && cl->fg != cur_fg) {
            n = snprintf(tmp, sizeof tmp, "\x1b[38;5;%dm", cl->fg);
            if (n > 0 && (size_t)n < sizeof tmp)
                buf_add(b, tmp, (size_t)n);
            cur_fg = cl->fg;
        }
        buf_add(b, &cl->ch, 1);
    }
    buf_str(b, "\x1b[0m");
}

static void add_centered(struct buf *b, int row, int cols, const char *text)
{
    int len = (int)strlen(text);
    if (cols < 1 || row < 1)
        return;
    if (len > cols - 1)
        len = cols - 1;
    if (len < 1)
        return;
    buf_fmt(b, "\x1b[%d;%dH", row, (cols - len) / 2 + 1);
    buf_add(b, text, (size_t)len);
}

static int draw_frame(int idx, int rows, int cols, double aspect)
{
    struct buf b = { NULL, 0, 0, 0 };
    struct cell *cells = NULL;
    const int hint = g_show_hint && rows >= 12 && cols >= 30;
    const int area = rows - (hint ? 1 : 0);
    int r, c, rc = 0;

    buf_str(&b, "\x1b[0m\x1b[2J");

    if (load_scheme(idx) == 0 &&
        fit_art(&pf_schemes[idx], area, cols, aspect, &r, &c)) {
        cells = malloc((size_t)r * (size_t)c * sizeof *cells);
        if (cells) {
            int top = (area - r) / 2;
            int left = (cols - c) / 2;
            render_cells(cells, r, c);
            for (int y = 0; y < r; y++)
                emit_row(&b, cells + (size_t)y * (size_t)c, c, top, left, y);
        }
    } else {
        add_centered(&b, rows / 2 + 1, cols, "peafb: terminal too small");
    }

    if (hint) {
        char msg[96];
        int n = snprintf(msg, sizeof msg,
                         "[%d/%d] <- -> / 1-%d: switch   q / Esc: quit",
                         idx + 1, pf_nschemes, pf_nschemes);
        if (n > 0 && (size_t)n < sizeof msg) {
            buf_str(&b, "\x1b[2m");
            add_centered(&b, rows, cols, msg);
            buf_str(&b, "\x1b[0m");
        }
    }

    if (!b.failed && b.p)
        rc = write_all(STDOUT_FILENO, b.p, b.n);
    free(cells);
    free(b.p);
    return rc;
}

static void get_term_size(int *rows, int *cols, double *aspect)
{
    struct winsize ws;
    int r = 0, c = 0;
    double a = DEFAULT_ASPECT;

    memset(&ws, 0, sizeof ws);
    if (ioctl(STDOUT_FILENO, TIOCGWINSZ, &ws) == 0 && ws.ws_row > 0 && ws.ws_col > 0) {
        r = ws.ws_row;
        c = ws.ws_col;
        if (ws.ws_xpixel > 0 && ws.ws_ypixel > 0) {
            double t = ((double)ws.ws_ypixel / ws.ws_row)
                     / ((double)ws.ws_xpixel / ws.ws_col);
            if (t >= 1.2 && t <= 3.5)
                a = t;
        }
    } else {
        const char *e;
        char *end;
        long v;
        if ((e = getenv("LINES")) != NULL) {
            v = strtol(e, &end, 10);
            if (end != e && v > 0 && v < 10000) r = (int)v;
        }
        if ((e = getenv("COLUMNS")) != NULL) {
            v = strtol(e, &end, 10);
            if (end != e && v > 0 && v < 10000) c = (int)v;
        }
        if (r <= 0) r = 24;
        if (c <= 0) c = 80;
    }
    *rows = r;
    *cols = c;
    *aspect = g_aspect_override > 0.0 ? g_aspect_override : a;
}

static void restore_terminal(void)
{
    if (g_screen_active) {
        static const char seq[] = "\x1b[0m\x1b[?25h\x1b[?1049l";
        (void)write_all(STDOUT_FILENO, seq, sizeof seq - 1);
        g_screen_active = 0;
    }
    if (g_have_termios) {
        (void)tcsetattr(STDIN_FILENO, TCSAFLUSH, &g_orig_termios);
        g_have_termios = 0;
    }
}

static void on_signal(int sig)
{
    if (sig == SIGWINCH || sig == SIGCONT)
        g_resize = 1;
    else
        g_quit = 1;
}

static int print_once(int idx, int color)
{
    int rows, cols, r, c, left;
    double aspect;
    struct cell *cells;
    struct buf b = { NULL, 0, 0, 0 };
    int rc = 0;

    get_term_size(&rows, &cols, &aspect);
    if (load_scheme(idx) != 0)
        return -1;
    if (!fit_art(&pf_schemes[idx], rows, cols, aspect, &r, &c))
        return 0;

    cells = malloc((size_t)r * (size_t)c * sizeof *cells);
    if (!cells)
        return -1;
    render_cells(cells, r, c);
    left = (cols - c) / 2;

    for (int y = 0; y < r; y++) {
        const struct cell *line = cells + (size_t)y * (size_t)c;
        int cur_fg = -2, cur_bg = -2, last = c - 1;

        if (!color) {
            while (last >= 0 && plain_char(&line[last]) == ' ')
                last--;
            if (last >= 0) {
                for (int i = 0; i < left; i++)
                    buf_add(&b, " ", 1);
                for (int x = 0; x <= last; x++) {
                    char ch = plain_char(&line[x]);
                    buf_add(&b, &ch, 1);
                }
            }
        } else {
            while (last >= 0 && line[last].ch == ' ' &&
                   line[last].bg == COL_DEFAULT)
                last--;
            if (last >= 0) {
                for (int i = 0; i < left; i++)
                    buf_add(&b, " ", 1);
                for (int x = 0; x <= last; x++) {
                    char tmp[24];
                    int n;
                    if (line[x].bg != cur_bg) {
                        if (line[x].bg == COL_DEFAULT)
                            n = snprintf(tmp, sizeof tmp, "\x1b[49m");
                        else
                            n = snprintf(tmp, sizeof tmp, "\x1b[48;5;%dm",
                                         line[x].bg);
                        if (n > 0 && (size_t)n < sizeof tmp)
                            buf_add(&b, tmp, (size_t)n);
                        cur_bg = line[x].bg;
                    }
                    if (line[x].ch != ' ' && line[x].fg != cur_fg) {
                        n = snprintf(tmp, sizeof tmp, "\x1b[38;5;%dm",
                                     line[x].fg);
                        if (n > 0 && (size_t)n < sizeof tmp)
                            buf_add(&b, tmp, (size_t)n);
                        cur_fg = line[x].fg;
                    }
                    buf_add(&b, &line[x].ch, 1);
                }
                buf_str(&b, "\x1b[0m");
            }
        }
        buf_add(&b, "\n", 1);
    }
    if (b.failed || !b.p || write_all(STDOUT_FILENO, b.p, b.n) != 0)
        rc = -1;
    free(b.p);
    free(cells);
    return rc;
}

enum { ACT_NONE, ACT_QUIT, ACT_NEXT, ACT_PREV, ACT_SELECT };

struct input {
    unsigned char d[64];
    size_t n;
};

static int wait_more(struct input *in, const sigset_t *mask, int ms)
{
    fd_set rf;
    struct timespec ts;
    int n;

    if (in->n >= sizeof in->d)
        return 0;
    FD_ZERO(&rf);
    FD_SET(STDIN_FILENO, &rf);
    ts.tv_sec = 0;
    ts.tv_nsec = (long)ms * 1000000L;
    n = pselect(STDIN_FILENO + 1, &rf, NULL, NULL, &ts, mask);
    if (n < 0)
        return errno == EINTR ? -1 : 0;
    if (n == 0)
        return 0;
    {
        ssize_t k = read(STDIN_FILENO, in->d + in->n, sizeof in->d - in->n);
        if (k <= 0)
            return k < 0 && (errno == EINTR || errno == EAGAIN) ? -1 : 0;
        in->n += (size_t)k;
    }
    return 1;
}

static void in_drop(struct input *in, size_t k)
{
    if (k >= in->n) {
        in->n = 0;
        return;
    }
    memmove(in->d, in->d + k, in->n - k);
    in->n -= k;
}

static int next_action(struct input *in, const sigset_t *mask, int *arg)
{
    while (in->n > 0) {
        unsigned char c = in->d[0];

        if (c == 27) {
            size_t j;
            if (in->n == 1) {
                int w = wait_more(in, mask, 50);
                if (w < 0)
                    return ACT_NONE;
                if (w == 0) {
                    in_drop(in, 1);
                    return ACT_QUIT;
                }
                continue;
            }
            if (in->d[1] == '[' || in->d[1] == 'O') {
                int final;
                j = 2;
                while (j < in->n && !(in->d[j] >= 0x40 && in->d[j] <= 0x7e))
                    j++;
                if (j >= in->n) {
                    int w;
                    if (in->n >= sizeof in->d) {
                        in->n = 0;
                        return ACT_NONE;
                    }
                    w = wait_more(in, mask, 50);
                    if (w < 0)
                        return ACT_NONE;
                    if (w == 0) {
                        in->n = 0;
                        return ACT_NONE;
                    }
                    continue;
                }
                final = in->d[j];
                in_drop(in, j + 1);
                if (final == 'C')
                    return ACT_NEXT;
                if (final == 'D')
                    return ACT_PREV;
                continue;
            }
            in_drop(in, 2);
            continue;
        }

        in_drop(in, 1);
        if (c == 'q' || c == 'Q' || c == 3 || c == 4)
            return ACT_QUIT;
        if (c == 'l' || c == 'L')
            return ACT_NEXT;
        if (c == 'h' || c == 'H')
            return ACT_PREV;
        if (c >= '1' && c <= '9' && (int)(c - '1') < pf_nschemes) {
            *arg = c - '1';
            return ACT_SELECT;
        }
    }
    return ACT_NONE;
}

static int run_tui(int idx)
{
    static const int sigs[] = { SIGINT, SIGTERM, SIGHUP, SIGWINCH, SIGCONT };
    struct termios raw;
    struct sigaction sa;
    sigset_t block, old;
    struct input in = { { 0 }, 0 };
    int rows = 0, cols = 0, last_rows = -1, last_cols = -1;
    int redraw = 1, stdin_open = 1, rc = 0;
    double aspect = DEFAULT_ASPECT;

    if (tcgetattr(STDIN_FILENO, &g_orig_termios) != 0) {
        perror("peafb: tcgetattr");
        return 1;
    }
    g_have_termios = 1;
    atexit(restore_terminal);

    raw = g_orig_termios;
    raw.c_lflag &= (tcflag_t)~(ICANON | ECHO | ISIG | IEXTEN);
    raw.c_iflag &= (tcflag_t)~IXON;
    raw.c_cc[VMIN] = 1;
    raw.c_cc[VTIME] = 0;
    if (tcsetattr(STDIN_FILENO, TCSAFLUSH, &raw) != 0) {
        perror("peafb: tcsetattr");
        g_have_termios = 0;
        return 1;
    }

    memset(&sa, 0, sizeof sa);
    sa.sa_handler = on_signal;
    sigemptyset(&sa.sa_mask);
    sigemptyset(&block);
    for (size_t i = 0; i < sizeof sigs / sizeof sigs[0]; i++) {
        sigaction(sigs[i], &sa, NULL);
        sigaddset(&block, sigs[i]);
    }
    signal(SIGPIPE, SIG_IGN);

    sigprocmask(SIG_BLOCK, &block, &old);

    {
        static const char enter[] = "\x1b[?1049h\x1b[?25l";
        g_screen_active = 1;
        if (write_all(STDOUT_FILENO, enter, sizeof enter - 1) != 0)
            rc = 1;
    }

    while (!g_quit && rc == 0) {
        fd_set rf;
        struct timespec ts;
        int nfds = 0, n;

        get_term_size(&rows, &cols, &aspect);

        if (g_resize || redraw || rows != last_rows || cols != last_cols) {
            g_resize = 0;
            redraw = 0;
            last_rows = rows;
            last_cols = cols;
            if (draw_frame(idx, rows, cols, aspect) != 0) {
                rc = 1;
                break;
            }
        }

        FD_ZERO(&rf);
        if (stdin_open) {
            FD_SET(STDIN_FILENO, &rf);
            nfds = STDIN_FILENO + 1;
        }
        ts.tv_sec = 1;
        ts.tv_nsec = 0;
        n = pselect(nfds, &rf, NULL, NULL, &ts, &old);
        if (n < 0) {
            if (errno == EINTR)
                continue;
            rc = 1;
            break;
        }
        if (n > 0 && stdin_open && FD_ISSET(STDIN_FILENO, &rf)) {
            ssize_t k = read(STDIN_FILENO, in.d + in.n, sizeof in.d - in.n);
            if (k > 0) {
                int act, arg = 0;
                in.n += (size_t)k;
                while ((act = next_action(&in, &old, &arg)) != ACT_NONE) {
                    if (act == ACT_QUIT) {
                        g_quit = 1;
                        break;
                    }
                    if (act == ACT_NEXT)
                        idx = (idx + 1) % pf_nschemes;
                    else if (act == ACT_PREV)
                        idx = (idx + pf_nschemes - 1) % pf_nschemes;
                    else if (act == ACT_SELECT)
                        idx = arg;
                    redraw = 1;
                }
            } else if (k == 0 || (errno != EINTR && errno != EAGAIN)) {
                stdin_open = 0;
            }
        }
    }

    restore_terminal();
    return rc;
}

static void usage(FILE *f)
{
    fputs("usage: peafb [-n NUM] [-a ASPECT] [-H]\n"
          "       peafb -o [-n NUM] [-C]\n"
          "       peafb -h | -V\n"
          "\n"
          "Boykisser ASCII art in the middle of the terminal.  It is redrawn\n"
          "when the terminal is resized.\n"
          "\n"
          "keys:  Left / Right (h / l)  previous / next picture\n"
          "       1 .. 6                pick a picture\n"
          "       q, Esc                quit\n"
          "\n"
          "  -n NUM     start with picture NUM (1-6)\n"
          "  -a ASPECT  cell height/width ratio, 1.0-4.0 (default: auto, 2.0)\n"
          "  -H         hide the status line\n"
          "  -o         print the picture once and exit\n"
          "  -C         with -o: use colours even when stdout is not a tty\n"
          "  -h         show this help\n"
          "  -V         show version\n", f);
}

int main(int argc, char **argv)
{
    int once = 0, force_color = 0, start = 0, opt, rc;

    while ((opt = getopt(argc, argv, "hVoCHn:a:")) != -1) {
        char *end;
        switch (opt) {
        case 'h':
            usage(stdout);
            return 0;
        case 'V':
            puts("peafb " VERSION);
            return 0;
        case 'o':
            once = 1;
            break;
        case 'C':
            force_color = 1;
            break;
        case 'H':
            g_show_hint = 0;
            break;
        case 'n': {
            long v = strtol(optarg, &end, 10);
            if (*optarg == '\0' || *end != '\0' || v < 1 || v > pf_nschemes) {
                fprintf(stderr, "peafb: invalid picture '%s' (use 1-%d)\n",
                        optarg, pf_nschemes);
                return 2;
            }
            start = (int)v - 1;
            break;
        }
        case 'a': {
            double v = strtod(optarg, &end);
            if (*optarg == '\0' || *end != '\0' || !(v >= 1.0 && v <= 4.0)) {
                fprintf(stderr, "peafb: invalid aspect '%s' (use 1.0-4.0)\n",
                        optarg);
                return 2;
            }
            g_aspect_override = v;
            break;
        }
        default:
            usage(stderr);
            return 2;
        }
    }
    if (optind < argc) {
        usage(stderr);
        return 2;
    }

    if (pf_nschemes < 1) {
        fprintf(stderr, "peafb: no pictures built in\n");
        return 1;
    }
    if (load_scheme(start) != 0) {
        fprintf(stderr, "peafb: failed to decode the embedded picture\n");
        return 1;
    }

    if (once || !isatty(STDIN_FILENO) || !isatty(STDOUT_FILENO)) {
        int color = force_color || isatty(STDOUT_FILENO);
        rc = print_once(start, color) == 0 ? 0 : 1;
    } else {
        rc = run_tui(start);
    }

    free_sat();
    return rc;
}
