#include "capture.h"

#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <ctime>

#include <unistd.h>
#include <sys/ioctl.h>
#include <signal.h>

static volatile bool g_running = true;
static void on_signal(int) { g_running = false; }

static void get_terminal_size(int& cols, int& rows) {
    winsize ws{};
    if (ioctl(STDOUT_FILENO, TIOCGWINSZ, &ws) == 0 && ws.ws_col > 0) {
        cols = ws.ws_col;
        rows = ws.ws_row;
    } else {
        cols = 80;
        rows = 24;
    }
}

// Map grayscale 0-255 to ANSI 256-color grayscale ramp (indices 232-255)
inline int gray_to_ansi(uint8_t v) {
    return 232 + (int)v * 23 / 255;
}

// Box-filter downsample: src (src_w x src_h) → dst (dst_w x dst_h)
static void downsample(const uint8_t* src, int src_w, int src_h,
                       uint8_t* dst,       int dst_w, int dst_h)
{
    float sx = (float)src_w / dst_w;
    float sy = (float)src_h / dst_h;

    for (int dy = 0; dy < dst_h; dy++) {
        for (int dx = 0; dx < dst_w; dx++) {
            int x0 = (int)(dx * sx),       x1 = (int)((dx + 1) * sx);
            int y0 = (int)(dy * sy),        y1 = (int)((dy + 1) * sy);
            if (x1 >= src_w) x1 = src_w - 1;
            if (y1 >= src_h) y1 = src_h - 1;

            int sum = 0, count = 0;
            for (int sy2 = y0; sy2 <= y1; sy2++)
                for (int sx2 = x0; sx2 <= x1; sx2++) {
                    sum += src[sy2 * src_w + sx2];
                    count++;
                }
            dst[dy * dst_w + dx] = count ? (uint8_t)(sum / count) : 0;
        }
    }
}

// Render pixels to ANSI terminal using the half-block ▄ trick.
// Each character row encodes 2 pixel rows:
//   background color = top pixel,  foreground color + ▄ = bottom pixel
static void render_frame(const uint8_t* pixels, int px_w, int px_h,
                         char* outbuf, size_t& outlen)
{
    char* p = outbuf;
    p += sprintf(p, "\e[H");  // cursor home — no clear, no flicker

    for (int row = 0; row < px_h; row += 2) {
        int next_row = (row + 1 < px_h) ? row + 1 : row;
        int prev_bg = -1, prev_fg = -1;

        for (int col = 0; col < px_w; col++) {
            int bg = gray_to_ansi(pixels[row      * px_w + col]); // top
            int fg = gray_to_ansi(pixels[next_row * px_w + col]); // bottom

            // Only emit escape codes when colors actually change
            if (bg != prev_bg && fg != prev_fg)
                p += sprintf(p, "\e[48;5;%d;38;5;%dm", bg, fg);
            else if (bg != prev_bg)
                p += sprintf(p, "\e[48;5;%dm", bg);
            else if (fg != prev_fg)
                p += sprintf(p, "\e[38;5;%dm", fg);

            prev_bg = bg;
            prev_fg = fg;

            // U+2584 LOWER HALF BLOCK — UTF-8: E2 96 84
            *p++ = (char)0xE2;
            *p++ = (char)0x96;
            *p++ = (char)0x84;
        }
        p += sprintf(p, "\e[0m\n");
    }
    outlen = (size_t)(p - outbuf);
}

static float compute_fps() {
    static int    frames = 0;
    static double t0     = 0.0;
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    double now = ts.tv_sec + ts.tv_nsec * 1e-9;
    if (t0 == 0.0) t0 = now;
    frames++;
    double elapsed = now - t0;
    if (elapsed < 1.0) return 0.0f;
    float fps = frames / elapsed;
    frames = 0; t0 = now;
    return fps;
}

int main(int argc, char* argv[]) {
    const char* dev = (argc > 1) ? argv[1] : "/dev/video0";

    signal(SIGINT,  on_signal);
    signal(SIGTERM, on_signal);

    fprintf(stderr, "Opening %s at 1920x1080...\n", dev);
    Camera cam(dev, 1920, 1080);

    // Hide cursor + switch to alternate screen (restored on exit)
    printf("\e[?25l\e[?1049h");
    fflush(stdout);

    // Discard warm-up frames (auto-exposure settling)
    for (int i = 0; i < 5; i++) cam.grab();

    // Scratch buffers — sized for the largest expected terminal
    uint8_t* pixels = new uint8_t[600 * 300];     // max ~600 cols x 300 px rows
    char*    outbuf = new char[600 * 300 * 26];   // ~26 bytes per cell worst case

    float fps = 0.0f;

    while (g_running) {
        int cols, rows;
        get_terminal_size(cols, rows);

        // Reserve bottom row for status line
        int render_rows = rows - 1;
        if (render_rows < 1) render_rows = 1;

        int px_w = cols;
        int px_h = render_rows * 2;  // half-block gives 2 pixels per char row

        // Clamp to buffer size
        if (px_w > 600) px_w = 600;
        if (px_h > 300) px_h = 300;

        Frame f = cam.grab();
        downsample(f.gray, f.width, f.height, pixels, px_w, px_h);

        size_t outlen = 0;
        render_frame(pixels, px_w, px_h, outbuf, outlen);

        // One big write per frame minimises tearing
        write(STDOUT_FILENO, outbuf, outlen);

        float new_fps = compute_fps();
        if (new_fps > 0.0f) fps = new_fps;

        // Status bar on the last row
        dprintf(STDOUT_FILENO,
            "\e[%d;1H\e[0m\e[7m  cam %dx%d → term %dx%d → pixels %dx%d  fps %.1f  Ctrl+C to quit  \e[0m",
            rows, f.width, f.height, cols, rows, px_w, px_h, fps);
    }

    // Restore terminal state
    printf("\e[?25h\e[?1049l\e[0m\n");
    printf("Bye.\n");

    delete[] pixels;
    delete[] outbuf;
    return 0;
}