#pragma once

#include <cstdint>
#include <cstddef>

// A grabbed frame.
// `data` points directly into the kernel-mapped buffer — do NOT free it.
// `gray` is allocated by Camera::grab() and owned by Camera — do NOT free it.
// Both are only valid until the next call to grab().
struct Frame {
    uint8_t* yuyv;   // raw YUYV packed bytes  (width * height * 2 bytes)
    uint8_t* gray;   // extracted Y plane only (width * height bytes)
    int      width;
    int      height;
};

class Camera {
public:
    // dev   : e.g. "/dev/video0"
    // width / height : requested resolution — driver may round to nearest supported
    Camera(const char* dev, int width, int height);
    ~Camera();

    // Grab the next frame from the driver queue.
    // Blocks until a frame is ready (typically < 33ms at 30fps).
    // Returns a Frame whose pointers are valid until the next grab() call.
    Frame grab();

    int width()  const { return m_width;  }
    int height() const { return m_height; }

private:
    int      m_fd;          // file descriptor for /dev/videoX
    void*    m_buf;         // mmap'd kernel buffer
    size_t   m_buf_len;     // length of that buffer
    uint8_t* m_gray;        // scratch buffer for Y extraction
    int      m_width;
    int      m_height;
    uint32_t m_buf_index;   // index of currently dequeued buffer

    void     init_device(const char* dev);
    void     init_mmap();
    void     start_streaming();
    void     yuyv_to_gray(const uint8_t* src, uint8_t* dst, int n_pixels);
};