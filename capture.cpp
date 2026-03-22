#include "capture.h"

#include <linux/videodev2.h>  // V4L2 structs and ioctl codes
#include <fcntl.h>            // open()
#include <unistd.h>           // close()
#include <sys/ioctl.h>        // ioctl()
#include <sys/mman.h>         // mmap(), munmap()

#include <cstring>            // memset()
#include <cstdlib>            // exit()
#include <cstdio>             // perror(), fprintf()
#include <cerrno>             // errno, EINTR

// ---------------------------------------------------------------------------
// Helper: wraps ioctl so EINTR (signal interruption) retries automatically
// ---------------------------------------------------------------------------
static int xioctl(int fd, unsigned long req, void* arg) {
    int r;
    do { r = ioctl(fd, req, arg); } while (r == -1 && errno == EINTR);
    return r;
}

// ---------------------------------------------------------------------------
// Constructor — open device, configure format, map buffer, start streaming
// ---------------------------------------------------------------------------
Camera::Camera(const char* dev, int width, int height)
    : m_fd(-1), m_buf(MAP_FAILED), m_buf_len(0),
      m_gray(nullptr), m_width(width), m_height(height), m_buf_index(0)
{
    init_device(dev);
    init_mmap();
    start_streaming();
    m_gray = new uint8_t[m_width * m_height];
}

// ---------------------------------------------------------------------------
// Destructor — stop streaming, unmap buffer, close fd
// ---------------------------------------------------------------------------
Camera::~Camera() {
    // Stop streaming
    v4l2_buf_type type = V4L2_BUF_TYPE_VIDEO_CAPTURE;
    xioctl(m_fd, VIDIOC_STREAMOFF, &type);

    // Unmap the kernel buffer
    if (m_buf != MAP_FAILED)
        munmap(m_buf, m_buf_len);

    // Close the device
    if (m_fd != -1)
        close(m_fd);

    delete[] m_gray;
}

// ---------------------------------------------------------------------------
// init_device — open /dev/videoX and set pixel format + resolution
// ---------------------------------------------------------------------------
void Camera::init_device(const char* dev) {
    // Open the device in read/write, non-blocking mode
    m_fd = open(dev, O_RDWR | O_NONBLOCK);
    if (m_fd == -1) {
        perror("open camera device");
        exit(1);
    }

    // Check it's actually a V4L2 capture device
    v4l2_capability cap{};
    if (xioctl(m_fd, VIDIOC_QUERYCAP, &cap) == -1) {
        perror("VIDIOC_QUERYCAP");
        exit(1);
    }
    if (!(cap.capabilities & V4L2_CAP_VIDEO_CAPTURE)) {
        fprintf(stderr, "Not a video capture device\n");
        exit(1);
    }
    if (!(cap.capabilities & V4L2_CAP_STREAMING)) {
        fprintf(stderr, "Device does not support streaming\n");
        exit(1);
    }

    // Request YUYV format at the given resolution.
    // YUYV (also called YUY2) packs two pixels into 4 bytes:
    //   [Y0 U0 Y1 V0] — Y is luma (brightness), U/V are chroma (color)
    // We only need Y for grayscale, so we just read every other byte.
    v4l2_format fmt{};
    fmt.type                = V4L2_BUF_TYPE_VIDEO_CAPTURE;
    fmt.fmt.pix.width       = m_width;
    fmt.fmt.pix.height      = m_height;
    fmt.fmt.pix.pixelformat = V4L2_PIX_FMT_YUYV;
    fmt.fmt.pix.field       = V4L2_FIELD_NONE;

    if (xioctl(m_fd, VIDIOC_S_FMT, &fmt) == -1) {
        perror("VIDIOC_S_FMT");
        exit(1);
    }

    // Driver may have adjusted the resolution — store what it actually gave us
    m_width  = fmt.fmt.pix.width;
    m_height = fmt.fmt.pix.height;

    fprintf(stderr, "[camera] %s opened: %dx%d YUYV\n", dev, m_width, m_height);
}

// ---------------------------------------------------------------------------
// init_mmap — ask the driver to allocate ONE buffer in kernel space,
//             then map it into our process address space with mmap().
//             This is zero-copy: grab() just gives us a pointer into
//             the kernel buffer — no memcpy needed.
// ---------------------------------------------------------------------------
void Camera::init_mmap() {
    // Request 1 buffer from the driver
    v4l2_requestbuffers req{};
    req.count  = 1;
    req.type   = V4L2_BUF_TYPE_VIDEO_CAPTURE;
    req.memory = V4L2_MEMORY_MMAP;

    if (xioctl(m_fd, VIDIOC_REQBUFS, &req) == -1) {
        perror("VIDIOC_REQBUFS");
        exit(1);
    }
    if (req.count == 0) {
        fprintf(stderr, "Driver returned 0 buffers\n");
        exit(1);
    }

    // Query the buffer to find out its size and offset in kernel memory
    v4l2_buffer buf{};
    buf.type   = V4L2_BUF_TYPE_VIDEO_CAPTURE;
    buf.memory = V4L2_MEMORY_MMAP;
    buf.index  = 0;

    if (xioctl(m_fd, VIDIOC_QUERYBUF, &buf) == -1) {
        perror("VIDIOC_QUERYBUF");
        exit(1);
    }

    m_buf_len = buf.length;

    // Map the kernel buffer into our virtual address space.
    // PROT_READ | PROT_WRITE  — we can read frame data
    // MAP_SHARED              — we share the same physical pages with the driver
    m_buf = mmap(nullptr, buf.length,
                 PROT_READ | PROT_WRITE, MAP_SHARED,
                 m_fd, buf.m.offset);

    if (m_buf == MAP_FAILED) {
        perror("mmap");
        exit(1);
    }
}

// ---------------------------------------------------------------------------
// start_streaming — enqueue the buffer, then tell the driver to start
// ---------------------------------------------------------------------------
void Camera::start_streaming() {
    // Put our buffer into the driver's incoming queue
    v4l2_buffer buf{};
    buf.type   = V4L2_BUF_TYPE_VIDEO_CAPTURE;
    buf.memory = V4L2_MEMORY_MMAP;
    buf.index  = 0;

    if (xioctl(m_fd, VIDIOC_QBUF, &buf) == -1) {
        perror("VIDIOC_QBUF");
        exit(1);
    }

    // Start the capture stream
    v4l2_buf_type type = V4L2_BUF_TYPE_VIDEO_CAPTURE;
    if (xioctl(m_fd, VIDIOC_STREAMON, &type) == -1) {
        perror("VIDIOC_STREAMON");
        exit(1);
    }
}

// ---------------------------------------------------------------------------
// grab — block until a frame is ready, dequeue it, extract Y plane,
//         then immediately re-enqueue the buffer for the next frame.
// ---------------------------------------------------------------------------
Frame Camera::grab() {
    // The device was opened O_NONBLOCK, so we use select() to block
    // until the driver has a frame ready. This avoids busy-waiting.
    fd_set fds;
    FD_ZERO(&fds);
    FD_SET(m_fd, &fds);

    // Timeout: 2 seconds (should never hit at normal framerates)
    timeval tv{};
    tv.tv_sec  = 2;
    tv.tv_usec = 0;

    int r = select(m_fd + 1, &fds, nullptr, nullptr, &tv);
    if (r == -1) { perror("select"); exit(1); }
    if (r ==  0) { fprintf(stderr, "Camera timeout\n"); exit(1); }

    // Dequeue the filled buffer — this gives us the frame
    v4l2_buffer buf{};
    buf.type   = V4L2_BUF_TYPE_VIDEO_CAPTURE;
    buf.memory = V4L2_MEMORY_MMAP;

    if (xioctl(m_fd, VIDIOC_DQBUF, &buf) == -1) {
        perror("VIDIOC_DQBUF");
        exit(1);
    }

    m_buf_index = buf.index;

    // Extract the Y (luma) plane from YUYV.
    // YUYV layout: [Y0 U Y1 V Y2 U Y3 V ...]
    // Y is at byte offsets 0, 2, 4, 6, ... — every even byte.
    yuyv_to_gray(static_cast<uint8_t*>(m_buf), m_gray, m_width * m_height);

    // Re-enqueue the buffer immediately so the driver can fill it again
    if (xioctl(m_fd, VIDIOC_QBUF, &buf) == -1) {
        perror("VIDIOC_QBUF requeue");
        exit(1);
    }

    return Frame{
        static_cast<uint8_t*>(m_buf),  // raw YUYV
        m_gray,                         // extracted grayscale
        m_width,
        m_height
    };
}

// ---------------------------------------------------------------------------
// yuyv_to_gray — extract Y channel from packed YUYV
// n_pixels = width * height
// src has 2 bytes per pixel: [Y U Y V Y U Y V ...]
// dst has 1 byte per pixel: just the Y values
// ---------------------------------------------------------------------------
void Camera::yuyv_to_gray(const uint8_t* src, uint8_t* dst, int n_pixels) {
    // Y byte is at every even index: 0, 2, 4, ...
    for (int i = 0; i < n_pixels; i++)
        dst[i] = src[i * 2];
}