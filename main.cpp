#include "capture.h"

#include <cstdio>
#include <cstdlib>

// ---------------------------------------------------------------------------
// Save a grayscale frame as a PGM file.
// PGM (Portable GrayMap) is the simplest image format:
//   header: "P5\n<width> <height>\n255\n"
//   body:   raw bytes, one byte per pixel
// Open with: feh frame_0.pgm   OR   eog frame_0.pgm   OR   display frame_0.pgm
// ---------------------------------------------------------------------------
static void save_pgm(const char* path, const uint8_t* gray, int w, int h) {
    FILE* f = fopen(path, "wb");
    if (!f) { perror("fopen"); return; }
    fprintf(f, "P5\n%d %d\n255\n", w, h);
    fwrite(gray, 1, w * h, f);
    fclose(f);
    fprintf(stderr, "[saved] %s\n", path);
}

// ---------------------------------------------------------------------------
// ASCII art preview — prints a tiny downsampled version of the frame
// to the terminal so you can verify it's not garbage without a GUI.
// ---------------------------------------------------------------------------
static void print_ascii(const uint8_t* gray, int w, int h) {
    const char* shades = " .:-=+*#%@";
    const int shade_count = 10;
    const int step_x = w / 80;
    const int step_y = h / 24;

    for (int y = 0; y < h; y += step_y) {
        for (int x = 0; x < w; x += step_x) {
            uint8_t p = gray[y * w + x];
            putchar(shades[p * (shade_count - 1) / 255]);
        }
        putchar('\n');
    }
}

int main(int argc, char* argv[]) {
    const char* dev = (argc > 1) ? argv[1] : "/dev/video0";

    fprintf(stderr, "Opening camera: %s\n", dev);
    Camera cam(dev, 640, 480);

    // Warm up — discard the first couple of frames.
    // Most cameras return dark or partially-exposed frames right after
    // streaming starts while the auto-exposure settles.
    fprintf(stderr, "Warming up camera (discarding 5 frames)...\n");
    for (int i = 0; i < 5; i++) cam.grab();

    // Grab and save 3 frames as PGM
    for (int i = 0; i < 3; i++) {
        Frame f = cam.grab();

        char path[64];
        snprintf(path, sizeof(path), "frame_%d.pgm", i);
        save_pgm(path, f.gray, f.width, f.height);
    }

    // Print ASCII preview of the last frame
    fprintf(stderr, "\nASCII preview of frame_2.pgm:\n");
    Frame f = cam.grab();
    print_ascii(f.gray, f.width, f.height);

    fprintf(stderr, "\nDone. View frames with: feh frame_0.pgm\n");
    return 0;
}