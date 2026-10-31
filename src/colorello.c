/*
 * colorello - show the colour palette of your terminal in its centre.
 */

#define _POSIX_C_SOURCE 200809L

#include <errno.h>
#include <fcntl.h>
#include <poll.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ioctl.h>
#include <sys/select.h>
#include <termios.h>
#include <unistd.h>

#define VERSION "0.1"

#define NCOLORS        7
#define MAX_BW         64
#define BLOCK_ASPECT   2.2
#define NOTCH_H        0.25
#define DEFAULT_ASPECT 2.0
#define MARGIN_DIV     10

static volatile sig_atomic_t g_quit = 0;
static volatile sig_atomic_t g_redraw = 0;

static struct termios g_saved;
static int g_have_termios = 0;
static int g_in_fd = -1;
static int g_own_in_fd = 0;
static int g_screen_active = 0;

static int write_all(const char *p, size_t n)
{
    while (n > 0) {
        ssize_t w = write(STDOUT_FILENO, p, n);
        if (w < 0) {
            if (errno == EINTR)
                continue;
            if (errno == EAGAIN || errno == EWOULDBLOCK) {
                struct pollfd pf;
                pf.fd = STDOUT_FILENO;
                pf.events = POLLOUT;
                pf.revents = 0;
                (void)poll(&pf, 1, 100);
                continue;
            }
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

static void buf_cup(struct buf *b, int row, int col)
{
    char tmp[48];
    int n = snprintf(tmp, sizeof tmp, "\x1b[%d;%dH", row, col);

    if (n > 0 && (size_t)n < sizeof tmp)
        buf_add(b, tmp, (size_t)n);
}

static void buf_sgr(struct buf *b, int code)
{
    char tmp[32];
    int n;

    if (code == 0)
        n = snprintf(tmp, sizeof tmp, "\x1b[0m");
    else
        n = snprintf(tmp, sizeof tmp, "\x1b[0;%dm", code);
    if (n > 0 && (size_t)n < sizeof tmp)
        buf_add(b, tmp, (size_t)n);
}

struct layout {
    int ok;
    int bw;
    int bh;
    int nx;
    int ny;
    int gap;
    int width;
    int left;
    int top;
};

static void shape(int bw, double aspect, int plain, struct layout *L)
{
    const double vscale = 2.0 / aspect;
    int h, n = 0;

    L->bw = bw;
    L->gap = (bw + 1) / 2;
    L->nx = bw >= 3 ? (bw + 1) / 3 : 0;

    h = (int)(BLOCK_ASPECT * bw * vscale / 2.0 + 0.5);
    if (h < 1)
        h = 1;
    L->bh = 2 * h;

    if (L->nx > 0) {
        n = (int)(NOTCH_H * bw * vscale + 0.5);
        if (plain)
            n = 2 * ((n + 1) / 2);
        if (n < 1)
            n = plain ? 2 : 1;
    }
    if (L->bh < 2 * n + 2)
        L->bh = 2 * n + 2;
    L->ny = n;
    L->width = NCOLORS * bw + (NCOLORS - 1) * L->gap;
}

static void compute_layout(int rows, int cols, double aspect, int plain,
                           struct layout *L)
{
    memset(L, 0, sizeof *L);
    if (rows < 1 || cols < 1)
        return;

    for (int pass = 0; pass < 2; pass++) {
        int mc = pass == 0 ? cols / MARGIN_DIV : 0;
        int mr = pass == 0 ? rows / MARGIN_DIV : 0;
        int aw = cols - 2 * mc;
        int ah = 2 * (rows - 2 * mr);
        struct layout cur, best;
        int found = 0;

        memset(&cur, 0, sizeof cur);
        memset(&best, 0, sizeof best);
        for (int bw = 1; bw <= MAX_BW; bw++) {
            shape(bw, aspect, plain, &cur);
            if (cur.width > aw || cur.bh > ah)
                break;
            best = cur;
            found = 1;
        }
        if (found) {
            int h = best.bh / 2;

            *L = best;
            L->ok = 1;
            L->left = (cols - L->width) / 2;
            if (plain || L->nx == 0)
                L->top = 2 * ((rows - h) / 2);
            else
                L->top = rows - h;
            return;
        }
    }
}

static int block_on(const struct layout *L, int xin, int y)
{
    if (y < 0 || y >= L->bh || xin < 0 || xin >= L->bw)
        return 0;
    if (y < L->ny)
        return xin < L->bw - L->nx;
    if (y >= L->bh - L->ny)
        return xin >= L->nx;
    return 1;
}

static void build_frame(struct buf *b, int rows, int cols, double aspect,
                        int plain)
{
    static const char upper[] = "\xe2\x96\x80";   /* U+2580 */
    static const char lower[] = "\xe2\x96\x84";   /* U+2584 */
    struct layout L;

    buf_str(b, "\x1b[0m\x1b[2J");
    compute_layout(rows, cols, aspect, plain, &L);

    if (!L.ok) {
        static const char msg[] = "too small";
        const int len = (int)sizeof msg - 1;

        if (cols >= len && rows >= 1) {
            buf_cup(b, (rows + 1) / 2, (cols - len) / 2 + 1);
            buf_str(b, msg);
        }
        return;
    }

    {
        const int pitch = L.bw + L.gap;
        const int r0 = L.top / 2;
        const int r1 = (L.top + L.bh - 1) / 2;

        for (int r = r0; r <= r1; r++) {
            int cur = -1;

            if (r < 0 || r >= rows)
                continue;
            buf_cup(b, r + 1, L.left + 1);

            for (int x = 0; x < L.width; x++) {
                const int k = x / pitch;
                const int xin = x % pitch;
                const int ton = block_on(&L, xin, 2 * r - L.top);
                const int bon = block_on(&L, xin, 2 * r + 1 - L.top);
                int state;
                const char *glyph;

                if (ton && bon) {
                    state = 40 + (k + 1);
                    glyph = " ";
                } else if (plain || (!ton && !bon)) {
                    state = 0;
                    glyph = " ";
                } else if (ton) {
                    state = 30 + (k + 1);
                    glyph = upper;
                } else {
                    state = 30 + (k + 1);
                    glyph = lower;
                }
                if (state != cur) {
                    buf_sgr(b, state);
                    cur = state;
                }
                buf_str(b, glyph);
            }
            buf_str(b, "\x1b[0m");
        }
    }
}

static int draw(int rows, int cols, double aspect, int plain)
{
    struct buf b = { NULL, 0, 0, 0 };
    int rc = 0;

    build_frame(&b, rows, cols, aspect, plain);
    if (!b.failed && b.p)
        rc = write_all(b.p, b.n);
    free(b.p);
    return rc;
}

static void get_term_size(int *rows, int *cols, double *aspect)
{
    struct winsize ws;
    int r = 0, c = 0;
    double a = DEFAULT_ASPECT;

    memset(&ws, 0, sizeof ws);
    if (ioctl(STDOUT_FILENO, TIOCGWINSZ, &ws) == 0 && ws.ws_row > 0 &&
        ws.ws_col > 0) {
        r = ws.ws_row;
        c = ws.ws_col;
        if (ws.ws_xpixel > 0 && ws.ws_ypixel > 0) {
            double t = ((double)ws.ws_ypixel / ws.ws_row) /
                       ((double)ws.ws_xpixel / ws.ws_col);
            if (t >= 1.4 && t <= 3.0)
                a = t;
        }
    } else {
        const char *e;
        char *end;
        long v;

        if ((e = getenv("LINES")) != NULL) {
            v = strtol(e, &end, 10);
            if (end != e && v > 0 && v < 10000)
                r = (int)v;
        }
        if ((e = getenv("COLUMNS")) != NULL) {
            v = strtol(e, &end, 10);
            if (end != e && v > 0 && v < 10000)
                c = (int)v;
        }
        if (r <= 0)
            r = 24;
        if (c <= 0)
            c = 80;
    }
    *rows = r;
    *cols = c;
    *aspect = a;
}

static void restore_terminal(void)
{
    if (g_screen_active) {
        static const char seq[] = "\x1b[0m\x1b[?7h\x1b[?25h\x1b[?1049l";

        g_screen_active = 0;
        (void)write_all(seq, sizeof seq - 1);
    }
    if (g_have_termios) {
        g_have_termios = 0;
        (void)tcsetattr(g_in_fd, TCSADRAIN, &g_saved);
    }
    if (g_own_in_fd && g_in_fd >= 0) {
        close(g_in_fd);
        g_in_fd = -1;
        g_own_in_fd = 0;
    }
}

static void on_signal(int sig)
{
    if (sig == SIGWINCH || sig == SIGCONT)
        g_redraw = 1;
    else
        g_quit = 1;
}

static int term_init(void)
{
    struct termios raw;

    if (!isatty(STDOUT_FILENO)) {
        fputs("colorello: standard output is not a terminal\n", stderr);
        return -1;
    }
    if (isatty(STDIN_FILENO)) {
        g_in_fd = STDIN_FILENO;
    } else {
        g_in_fd = open("/dev/tty", O_RDONLY | O_CLOEXEC);
        if (g_in_fd < 0) {
            fputs("colorello: cannot access the terminal for input\n", stderr);
            return -1;
        }
        g_own_in_fd = 1;
    }
    if (tcgetattr(g_in_fd, &g_saved) != 0) {
        perror("colorello: tcgetattr");
        restore_terminal();
        return -1;
    }
    raw = g_saved;
    raw.c_lflag &= (tcflag_t)~(ICANON | ECHO | ISIG | IEXTEN);
    raw.c_iflag &= (tcflag_t)~(IXON | ICRNL | INLCR | IGNCR);
    raw.c_cc[VMIN] = 1;
    raw.c_cc[VTIME] = 0;
    if (tcsetattr(g_in_fd, TCSAFLUSH, &raw) != 0) {
        perror("colorello: tcsetattr");
        restore_terminal();
        return -1;
    }
    g_have_termios = 1;
    atexit(restore_terminal);
    return 0;
}

static int handle_input(int fd)
{
    unsigned char buf[64];
    ssize_t n;

    n = read(fd, buf, sizeof buf);
    if (n < 0)
        return (errno == EINTR || errno == EAGAIN) ? 0 : -1;
    if (n == 0)
        return -1;

    for (size_t i = 0; i < (size_t)n; i++) {
        unsigned char c = buf[i];

        if (c == 0x1b) {
            if (i + 1 == (size_t)n) {     
                struct pollfd pf;

                pf.fd = fd;
                pf.events = POLLIN;
                pf.revents = 0;
                if (poll(&pf, 1, 40) > 0) {
                    unsigned char rest[64];
                    ssize_t k = read(fd, rest, sizeof rest);

                    if (k == 0)
                        return -1;
                    return 0;
                }
                return 1;
            }
            return 0;
        }
        if (c == 'q' || c == 'Q' || c == 0x03 || c == 0x04)
            return 1;
        if (c == 0xD0 && i + 1 < (size_t)n &&
            (buf[i + 1] == 0xB9 || buf[i + 1] == 0x99))
            return 1;
        if (c == 0x0c)
            g_redraw = 1;
    }
    return 0;
}

static int run(int plain, double aspect_override)
{
    static const int sigs[] = { SIGINT, SIGTERM, SIGHUP, SIGQUIT,
                                SIGWINCH, SIGCONT };
    static const char enter[] =
        "\x1b[?1049h\x1b[?25l\x1b[?7l\x1b[0m\x1b[2J";
    struct sigaction sa;
    sigset_t block, old;
    int rows = 0, cols = 0, last_rows = -1, last_cols = -1;
    int input_open = 1, rc = 0;
    double aspect = DEFAULT_ASPECT, last_aspect = 0.0;

    if (term_init() != 0)
        return 1;

    memset(&sa, 0, sizeof sa);
    sigemptyset(&sa.sa_mask);
    sa.sa_handler = on_signal;
    sigemptyset(&block);
    for (size_t i = 0; i < sizeof sigs / sizeof sigs[0]; i++) {
        sigaction(sigs[i], &sa, NULL);
        sigaddset(&block, sigs[i]);
    }
    sa.sa_handler = SIG_IGN;
    sigaction(SIGPIPE, &sa, NULL);
    sigaction(SIGTSTP, &sa, NULL);
    sigaction(SIGTTIN, &sa, NULL);
    sigaction(SIGTTOU, &sa, NULL);

    sigprocmask(SIG_BLOCK, &block, &old);

    g_screen_active = 1;
    if (write_all(enter, sizeof enter - 1) != 0)
        rc = 1;

    while (!g_quit && rc == 0) {
        fd_set rf;
        struct timespec ts;
        int nfds = 0, n;

        get_term_size(&rows, &cols, &aspect);
        if (aspect_override > 0.0)
            aspect = aspect_override;

        if (g_redraw || rows != last_rows || cols != last_cols ||
            aspect != last_aspect) {
            g_redraw = 0;
            last_rows = rows;
            last_cols = cols;
            last_aspect = aspect;
            if (draw(rows, cols, aspect, plain) != 0) {
                rc = 1;
                break;
            }
        }

        FD_ZERO(&rf);
        if (input_open) {
            FD_SET(g_in_fd, &rf);
            nfds = g_in_fd + 1;
        }
        ts.tv_sec = 0;
        ts.tv_nsec = 500 * 1000 * 1000;
        n = pselect(nfds, &rf, NULL, NULL, &ts, &old);
        if (n < 0) {
            if (errno == EINTR)
                continue;
            rc = 1;
            break;
        }
        if (n > 0 && input_open && FD_ISSET(g_in_fd, &rf)) {
            int k = handle_input(g_in_fd);

            if (k > 0)
                break;
            if (k < 0)
                input_open = 0;
        }
    }

    restore_terminal();
    return rc;
}

static void usage(FILE *f)
{
    fputs("usage: colorello [-p] [-a ASPECT]\n"
          "       colorello -h | -V\n"
          "\n"
          "Shows the colour palette of the terminal in its centre.\n"
          "The picture follows the terminal size; q or Esc quits.\n"
          "\n"
          "  -p         plain mode: whole cells only, no half-block characters\n"
          "             (for fonts or consoles that lack them)\n"
          "  -a ASPECT  cell height/width ratio, 1.0-4.0 (default: auto, 2.0)\n"
          "  -h         show this help\n"
          "  -V         show version\n", f);
}

int main(int argc, char **argv)
{
    const char *term = getenv("TERM");
    double aspect = 0.0;
    int plain = 0, opt;

    if (term && strcmp(term, "linux") == 0)
        plain = 1;

    opterr = 0;
    while ((opt = getopt(argc, argv, ":hVpa:")) != -1) {
        switch (opt) {
        case 'h':
            usage(stdout);
            return 0;
        case 'V':
            puts("colorello " VERSION);
            return 0;
        case 'p':
            plain = 1;
            break;
        case 'a': {
            char *end;
            double v = strtod(optarg, &end);

            if (end == optarg || *end != '\0' || !(v >= 1.0 && v <= 4.0)) {
                fprintf(stderr, "colorello: invalid aspect '%s' "
                        "(expected 1.0-4.0)\n", optarg);
                return 2;
            }
            aspect = v;
            break;
        }
        case ':':
            fprintf(stderr, "colorello: option -%c requires an argument\n",
                    optopt);
            usage(stderr);
            return 2;
        default:
            if (optopt)
                fprintf(stderr, "colorello: invalid option -%c\n", optopt);
            else
                fprintf(stderr, "colorello: invalid option\n");
            usage(stderr);
            return 2;
        }
    }
    if (optind < argc) {
        fprintf(stderr, "colorello: unexpected argument '%s'\n", argv[optind]);
        usage(stderr);
        return 2;
    }

    return run(plain, aspect);
}
