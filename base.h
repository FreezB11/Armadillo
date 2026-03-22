// capture.h — minimal V4L2 frame grabber
#include <linux/videodev2.h>
#include <fcntl.h>
#include <sys/ioctl.h>
#include <sys/mman.h>

struct Frame { uint8_t* data; int width, height; };

class Camera {
    int fd; void* buf; size_t buf_len;
public:
    Camera(const char* dev, int w, int h);
    Frame grab();   // returns YUYV/MJPEG raw bytes
    ~Camera();
};