/*  video_device.c
 *
 *  Copyright (C) 2014-2026 Toxic All Rights Reserved.
 *
 *  This file is part of Toxic. Toxic is free software licensed
 *  under the GNU General Public License 3.0.
 */

#include "video_call.h"
#include "video_device.h"

#include <sys/ioctl.h>

#include <vpx/vpx_image.h>

/* Cameras come from V4L2, or from the backend that the camera_* macros map to. */
#if defined(__OSX__) || defined(__APPLE__)
#import "osx_video.h"
#define camera_init osx_video_init
#define camera_release osx_video_release
#define camera_open osx_video_open_device
#define camera_close osx_video_close_device
#define camera_read osx_video_read_device
#elif defined(__ANDROID__)
#include "android_video.h"
#define camera_init android_video_init
#define camera_release android_video_release
#define camera_open android_video_open_device
#define camera_close android_video_close_device
#define camera_read android_video_read_device
#else
#define USE_V4L2
#include <fcntl.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <X11/Xlib.h>
#include <X11/Xos.h>
#include <X11/Xutil.h>
#if defined(__OpenBSD__) || defined(__NetBSD__)
#include <sys/videoio.h>
#else
#include <linux/videodev2.h>
#endif /* defined(__OpenBSD__) || defined(__NetBSD__) */
#endif /* __OSX__ || __APPLE__ */

/* Video is shown in X11 windows, except on Android, where the app shows it. */
#ifndef __ANDROID__
#define USE_X11
#endif

#include "line_info.h"
#include "misc_tools.h"
#include "settings.h"

#include <errno.h>
#include <pthread.h>
#include <stdbool.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#ifdef VIDEO

struct VideoBuffer {
    void *start;
    size_t length;
};

typedef struct VideoDevice {
    VideoDataHandleCallback cb;             /* Use this to handle data from input device usually */
    void *cb_data;                          /* Data to be passed to callback */
    int32_t friend_number;                  /* ToxAV friend number */

#ifdef USE_V4L2
    int fd;                                 /* File descriptor of video device selected/opened */
    struct v4l2_format fmt;
    struct VideoBuffer *buffers;
    uint32_t n_buffers;
#endif

    uint32_t ref_count;
    int32_t selection;
    pthread_mutex_t mutex[1];
    uint16_t video_width;
    uint16_t video_height;

    vpx_image_t input;

#ifdef USE_X11
    Display *x_display;
    Window x_window;
    GC x_gc;
#endif /* USE_X11 */

} VideoDevice;

static const char *dvideo_device_names[2];        /* Default device */
static char *video_devices_names[2][MAX_DEVICES]; /* Container of available devices */
static int c_size[2];                               /* Size of above containers */
static VideoDevice *video_devices_running[2][MAX_DEVICES] = {{NULL}}; /* Running devices */
static uint32_t primary_video_device[2];                              /* Primary device */

static ToxAV *av = NULL;

/* q_mutex */
#define lock pthread_mutex_lock(&video_mutex)
#define unlock pthread_mutex_unlock(&video_mutex)
static pthread_mutex_t video_mutex;

static bool video_thread_running = true;
static bool video_thread_paused = true;                /* Thread control */

void *video_thread_poll(void *userdata);

static void yuv420tobgr(uint16_t width, uint16_t height, const uint8_t *y,
                        const uint8_t *u, const uint8_t *v, unsigned int ystride,
                        unsigned int ustride, unsigned int vstride, uint8_t *out)
{
    unsigned long int i, j;

    for (i = 0; i < height; ++i) {
        for (j = 0; j < width; ++j) {
            uint8_t *point = out + 4 * ((i * width) + j);
            int t_y = y[((i * ystride) + j)];
            int t_u = u[(((i / 2) * ustride) + (j / 2))];
            int t_v = v[(((i / 2) * vstride) + (j / 2))];
            t_y = t_y < 16 ? 16 : t_y;

            int r = (298 * (t_y - 16) + 409 * (t_v - 128) + 128) >> 8;
            int g = (298 * (t_y - 16) - 100 * (t_u - 128) - 208 * (t_v - 128) + 128) >> 8;
            int b = (298 * (t_y - 16) + 516 * (t_u - 128) + 128) >> 8;

            point[2] = r > 255 ? 255 : r < 0 ? 0 : r;
            point[1] = g > 255 ? 255 : g < 0 ? 0 : g;
            point[0] = b > 255 ? 255 : b < 0 ? 0 : b;
            point[3] = ~0;
        }
    }
}

#ifdef USE_V4L2
static void yuv422to420(uint8_t *plane_y, uint8_t *plane_u, uint8_t *plane_v,
                        uint8_t *f_input, uint16_t width, uint16_t height)
{
    uint8_t *end = f_input + width * height * 2;

    while (f_input != end) {
        uint8_t *line_end = f_input + width * 2;

        while (f_input != line_end) {
            *plane_y++ = *f_input++;
            *plane_u++ = *f_input++;
            *plane_y++ = *f_input++;
            *plane_v++ = *f_input++;
        }

        line_end = f_input + width * 2;

        while (f_input != line_end) {
            *plane_y++ = *f_input++;
            f_input++;//u
            *plane_y++ = *f_input++;
            f_input++;//v
        }
    }
}

static int xioctl(int fh, unsigned long request, void *arg)
{
    int r;

    do {
        r = ioctl(fh, request, arg);
    } while (-1 == r && EINTR == errno);

    return r;
}

#endif /* USE_V4L2 */

#ifdef USE_X11
static bool video_window_open(VideoDevice *device, VideoDeviceType type, const char *title)
{
    UNUSED_VAR(type);

    if ((device->x_display = XOpenDisplay(NULL)) == NULL) {
        return false;
    }

    int screen = DefaultScreen(device->x_display);
    const uint16_t width = type == vdt_input ? device->video_width : 100;
    const uint16_t height = type == vdt_input ? device->video_height : 100;

    if (!(device->x_window = XCreateSimpleWindow(device->x_display, RootWindow(device->x_display, screen), 0, 0,
                             width, height, 0, BlackPixel(device->x_display, screen),
                             BlackPixel(device->x_display, screen)))) {
        return false;
    }

    XStoreName(device->x_display, device->x_window, title);
    XSelectInput(device->x_display, device->x_window, ExposureMask | ButtonPressMask | KeyPressMask);

    if ((device->x_gc = DefaultGC(device->x_display, screen)) == NULL) {
        return false;
    }

    /* Disable user from manually closing the X11 window */
    Atom wm_delete_window = XInternAtom(device->x_display, "WM_DELETE_WINDOW", false);
    XSetWMProtocols(device->x_display, device->x_window, &wm_delete_window, 1);

    XMapWindow(device->x_display, device->x_window);
    XClearWindow(device->x_display, device->x_window);
    XMapRaised(device->x_display, device->x_window);
    XFlush(device->x_display);
    return true;
}

static void video_window_resize(VideoDevice *device, uint16_t width, uint16_t height)
{
    XResizeWindow(device->x_display, device->x_window, width, height);
}

/* bgra is width * height pixels from yuv420tobgr. */
static void video_window_draw(VideoDevice *device, VideoDeviceType type, uint8_t *bgra, uint16_t width,
                              uint16_t height)
{
    UNUSED_VAR(type);

    /* Allocate image data in X11 */
    XImage image = {
        .width = width,
        .height = height,
        .depth = 24,
        .bits_per_pixel = 32,
        .format = ZPixmap,
        .byte_order = LSBFirst,
        .bitmap_unit = 8,
        .bitmap_bit_order = LSBFirst,
        .bytes_per_line = width * 4,
        .red_mask = 0xFF0000,
        .green_mask = 0xFF00,
        .blue_mask = 0xFF,
        .data = (char *)bgra
    };

    /* Render image data */
    Pixmap pixmap = XCreatePixmap(device->x_display, device->x_window, width, height, 24);
    XPutImage(device->x_display, pixmap, device->x_gc, &image, 0, 0, 0, 0, width, height);
    XCopyArea(device->x_display, pixmap, device->x_window, device->x_gc, 0, 0, width, height, 0, 0);
    XFreePixmap(device->x_display, pixmap);
    XFlush(device->x_display);
}

static void video_window_close(VideoDevice *device, VideoDeviceType type)
{
    UNUSED_VAR(type);

    if (device->x_display == NULL) {
        return;
    }

    if (device->x_window) {
        XDestroyWindow(device->x_display, device->x_window);
    }

    XFlush(device->x_display);
    XCloseDisplay(device->x_display);
}
#else /* not USE_X11 */
static Android_Video_Window android_window(VideoDeviceType type)
{
    return type == vdt_input ? ANDROID_VIDEO_PREVIEW : ANDROID_VIDEO_RECEIVE;
}

static bool video_window_open(VideoDevice *device, VideoDeviceType type, const char *title)
{
    UNUSED_VAR(device);
    UNUSED_VAR(title);
    android_video_window_open(android_window(type));
    return true;
}

static void video_window_resize(VideoDevice *device, uint16_t width, uint16_t height)
{
    UNUSED_VAR(device);
    UNUSED_VAR(width);
    UNUSED_VAR(height);
}

static void video_window_draw(VideoDevice *device, VideoDeviceType type, uint8_t *bgra, uint16_t width,
                              uint16_t height)
{
    UNUSED_VAR(device);
    android_video_window_draw(android_window(type), bgra, width, height);
}

static void video_window_close(VideoDevice *device, VideoDeviceType type)
{
    UNUSED_VAR(device);
    android_video_window_close(android_window(type));
}
#endif /* USE_X11 */

VideoDeviceError init_video_devices(Toxic *toxic)
{
    c_size[vdt_input] = 0;

#ifndef USE_V4L2

    if (camera_init(&video_devices_names[vdt_input][0], &c_size[vdt_input]) != 0) {
        return vde_InternalError;
    }

#else /* USE_V4L2 */

    for (; c_size[vdt_input] <= MAX_DEVICES; ++c_size[vdt_input]) {
        int fd;
        char device_address[MAX_STR_SIZE];
        snprintf(device_address, sizeof(device_address), "/dev/video%i", c_size[vdt_input]);

        fd = open(device_address, O_RDWR | O_NONBLOCK, 0);

        if (fd == -1) {
            break;
        } else {
            struct v4l2_capability cap;
            char *video_input_name;

            /* Query V4L for capture capabilities */
            if (-1 != ioctl(fd, VIDIOC_QUERYCAP, &cap)) {
                size_t video_input_name_len = strlen((const char *)cap.card) + strlen(device_address) + 4;
                video_input_name = malloc(video_input_name_len);

                if (video_input_name == NULL) {
                    close(fd);
                    return vde_InternalError;
                }

                snprintf(video_input_name, video_input_name_len, "%s (%s)", (char *)cap.card, device_address);
            } else {
                size_t video_input_name_len = strlen(device_address) + 3;
                video_input_name = malloc(video_input_name_len);

                if (video_input_name == NULL) {
                    close(fd);
                    return vde_InternalError;
                }

                snprintf(video_input_name, video_input_name_len, "(%s)", device_address);
            }

            video_devices_names[vdt_input][c_size[vdt_input]] = video_input_name;

            close(fd);
        }
    }

#endif

    c_size[vdt_output] = 1;
    // TODO(iphydf): String literals are const char *. This may need to be
    // copied, or if we're not owning any output device names, it should be
    // const and video_devices_names needs to be split.
    char *video_output_name = "Toxic Video Receiver";
    video_devices_names[vdt_output][0] = video_output_name;

    // Start poll thread
    if (pthread_mutex_init(&video_mutex, NULL) != 0) {
        return vde_InternalError;
    }

    pthread_t thread_id;

    if (pthread_create(&thread_id, NULL, video_thread_poll, (void *) toxic) != 0
            || pthread_detach(thread_id) != 0) {
        return vde_InternalError;
    }

#ifdef VIDEO
    av = toxic->av;
#endif /* VIDEO */

    return (VideoDeviceError) vde_None;
}

VideoDeviceError terminate_video_devices(void)
{
    /* Cleanup if needed */
    lock;
    video_thread_running = false;
    unlock;

    sleep_thread(20000L);

    int i;

    for (i = 0; i < c_size[vdt_input]; ++i) {
        free(video_devices_names[vdt_input][i]);
    }

    if (pthread_mutex_destroy(&video_mutex) != 0) {
        return (VideoDeviceError) vde_InternalError;
    }

#ifndef USE_V4L2
    camera_release();
#endif /* USE_V4L2 */

    return (VideoDeviceError) vde_None;
}

VideoDeviceError register_video_device_callback(int32_t friend_number, uint32_t device_idx,
        VideoDataHandleCallback callback, void *data)
{
#ifndef USE_V4L2

    if (c_size[vdt_input] <= device_idx || !video_devices_running[vdt_input][device_idx]) {
        return vde_InvalidSelection;
    }

#else /* USE_V4L2 */

    if (c_size[vdt_input] <= device_idx || !video_devices_running[vdt_input][device_idx]
            || !video_devices_running[vdt_input][device_idx]->fd) {
        return vde_InvalidSelection;
    }

#endif

    lock;
    video_devices_running[vdt_input][device_idx]->cb = callback;
    video_devices_running[vdt_input][device_idx]->cb_data = data;
    video_devices_running[vdt_input][device_idx]->friend_number = friend_number;
    unlock;

    return vde_None;
}

VideoDeviceError set_primary_video_device(VideoDeviceType type, int32_t selection)
{
    if (c_size[type] <= selection || selection < 0) {
        return vde_InvalidSelection;
    }

    primary_video_device[type] = selection;

    return vde_None;
}

VideoDeviceError open_primary_video_device(VideoDeviceType type, uint32_t *device_idx,
        uint32_t *width, uint32_t *height)
{
    return open_video_device(type, primary_video_device[type], device_idx, width, height);
}

void get_primary_video_device_name(VideoDeviceType type, char *buf, int size)
{
    memcpy(buf, dvideo_device_names[type], size);
}

VideoDeviceError open_video_device(VideoDeviceType type, int32_t selection, uint32_t *device_idx,
                                   uint32_t *width, uint32_t *height)
{
    if (c_size[type] <= selection || selection < 0) {
        return vde_InvalidSelection;
    }

    lock;

    uint32_t i, temp_idx = -1;

    for (i = 0; i < MAX_DEVICES; ++i) {
        if (!video_devices_running[type][i]) {
            temp_idx = i;
            break;
        }
    }

    if (temp_idx == -1) {
        unlock;
        return vde_AllDevicesBusy;
    }

    for (i = 0; i < MAX_DEVICES; i ++) { /* Check if any device has the same selection */
        if (video_devices_running[type][i] && video_devices_running[type][i]->selection == selection) {

            video_devices_running[type][temp_idx] = video_devices_running[type][i];
            video_devices_running[type][i]->ref_count++;

            unlock;
            return vde_None;
        }
    }

    VideoDevice *device = video_devices_running[type][temp_idx] = calloc(1, sizeof(VideoDevice));
    device->selection = selection;

    if (pthread_mutex_init(device->mutex, NULL) != 0) {
        free(device);
        unlock;
        return vde_InternalError;
    }

    if (type == vdt_input) {
        video_thread_paused = true;

#ifndef USE_V4L2

        /* TODO: use requested resolution */
        if (camera_open(selection, &device->video_width, &device->video_height) != 0) {
            video_devices_running[type][temp_idx] = NULL;
            free(device);
            unlock;
            return vde_FailedStart;
        }

#else /* USE_V4L2 */
        /* Open selected device */
        char device_address[MAX_STR_SIZE];
        snprintf(device_address, sizeof(device_address), "/dev/video%i", selection);

        device->fd = open(device_address, O_RDWR);

        if (device->fd == -1) {
            unlock;
            return vde_FailedStart;
        }

        /* Obtain video device capabilities */
        struct v4l2_capability cap;

        if (-1 == xioctl(device->fd, VIDIOC_QUERYCAP, &cap)) {
            close(device->fd);
            free(device);
            unlock;
            return vde_FailedStart;
        }

        /* Setup video format */
        struct v4l2_format fmt = {0};

        fmt.type = V4L2_BUF_TYPE_VIDEO_CAPTURE;
        fmt.fmt.pix.pixelformat = V4L2_PIX_FMT_YUYV;
        fmt.fmt.pix.width = width == NULL ? 0 : *width;
        fmt.fmt.pix.height = height == NULL ? 0 : *height;

        if (-1 == xioctl(device->fd, VIDIOC_S_FMT, &fmt)) {
            close(device->fd);
            free(device);
            unlock;
            return vde_FailedStart;
        }

        device->video_width = fmt.fmt.pix.width;
        device->video_height = fmt.fmt.pix.height;

        /* Request buffers */
        struct v4l2_requestbuffers req = {0};

        req.count = 4;
        req.type = V4L2_BUF_TYPE_VIDEO_CAPTURE;
        req.memory = V4L2_MEMORY_MMAP;

        if (-1 == xioctl(device->fd, VIDIOC_REQBUFS, &req)) {
            close(device->fd);
            free(device);
            unlock;
            return vde_FailedStart;
        }

        if (req.count < 2) {
            close(device->fd);
            free(device);
            unlock;
            return vde_FailedStart;
        }

        device->buffers = calloc(req.count, sizeof(struct VideoBuffer));

        for (i = 0; i < req.count; ++i) {
            struct v4l2_buffer buf = {0};

            buf.type = V4L2_BUF_TYPE_VIDEO_CAPTURE;
            buf.memory = V4L2_MEMORY_MMAP;
            buf.index = i;

            if (-1 == xioctl(device->fd, VIDIOC_QUERYBUF, &buf)) {
                close(device->fd);
                free(device);
                unlock;
                return vde_FailedStart;
            }

            device->buffers[i].length = buf.length;
            device->buffers[i].start = mmap(NULL /* start anywhere */,
                                            buf.length,
                                            PROT_READ | PROT_WRITE /* required */,
                                            MAP_SHARED /* recommended */,
                                            device->fd, buf.m.offset);

            if (MAP_FAILED == device->buffers[i].start) {
                for (i = 0; i < buf.index; ++i) {
                    munmap(device->buffers[i].start, device->buffers[i].length);
                }

                close(device->fd);
                free(device);
                unlock;
                return vde_FailedStart;
            }
        }

        device->n_buffers = i;

        enum v4l2_buf_type btype;

        for (i = 0; i < device->n_buffers; ++i) {
            struct v4l2_buffer buf = {0};

            buf.type = V4L2_BUF_TYPE_VIDEO_CAPTURE;
            buf.memory = V4L2_MEMORY_MMAP;
            buf.index = i;

            if (-1 == xioctl(device->fd, VIDIOC_QBUF, &buf)) {
                for (i = 0; i < device->n_buffers; ++i) {
                    munmap(device->buffers[i].start, device->buffers[i].length);
                }

                close(device->fd);
                free(device);
                unlock;
                return vde_FailedStart;
            }
        }

        btype = V4L2_BUF_TYPE_VIDEO_CAPTURE;

        /* Turn on video stream */
        if (-1 == xioctl(device->fd, VIDIOC_STREAMON, &btype)) {
            close_video_device(vdt_input, temp_idx);
            unlock;
            return vde_FailedStart;
        }

#endif /* USE_V4L2 */

        if (!video_window_open(device, vdt_input, "Video Preview")) {
            close_video_device(vdt_input, temp_idx);
            unlock;
            return vde_FailedStart;
        }

        vpx_img_alloc(&device->input, VPX_IMG_FMT_I420, device->video_width, device->video_height, 1);

        if (width != NULL) {
            *width = device->video_width;
        }

        if (height != NULL) {
            *height = device->video_height;
        }

        video_thread_paused = false;
    } else { /* vdt_output */

        if (!video_window_open(device, vdt_output, "Video Receive")) {
            close_video_device(vdt_output, temp_idx);
            unlock;
            return vde_FailedStart;
        }

        vpx_img_alloc(&device->input, VPX_IMG_FMT_I420, device->video_width, device->video_height, 1);
    }

    *device_idx = temp_idx;
    unlock;

    return vde_None;
}

VideoDeviceError write_video_out(uint16_t width, uint16_t height,
                                 uint8_t const *y, uint8_t const *u, uint8_t const *v,
                                 int32_t ystride, int32_t ustride, int32_t vstride,
                                 void *user_data)
{
    UNUSED_VAR(user_data);

    VideoDevice *device = video_devices_running[vdt_output][0];

    if (!device) {
        return vde_DeviceNotActive;
    }

#ifdef USE_X11

    if (!device->x_window) {
        return vde_DeviceNotActive;
    }

#endif /* USE_X11 */

    pthread_mutex_lock(device->mutex);

    /* Resize window to correct size */
    if (device->video_width != width || device->video_height != height) {
        device->video_width = width;
        device->video_height = height;
        video_window_resize(device, width, height);

        vpx_img_free(&device->input);
        vpx_img_alloc(&device->input, VPX_IMG_FMT_I420, width, height, 1);
    }

    /* Convert YUV420 data to BGR */
    ystride = abs(ystride);
    ustride = abs(ustride);
    vstride = abs(vstride);
    uint8_t *img_data = malloc(width * height * 4);
    yuv420tobgr(width, height, y, u, v, ystride, ustride, vstride, img_data);
    video_window_draw(device, vdt_output, img_data, width, height);
    free(img_data);

    pthread_mutex_unlock(device->mutex);
    return vde_None;
}

void *video_thread_poll(void *userdata)  // TODO: maybe use thread for every input source
{
    /*
     * NOTE: We only need to poll input devices for data.
     */
    Toxic *toxic = (Toxic *) userdata;

    if (toxic == NULL) {
        pthread_exit(NULL);
    }

    uint32_t i;

    while (1) {
        lock;

        if (!video_thread_running) {
            unlock;
            break;
        }

        unlock;

        if (video_thread_paused) {
            sleep_thread(10000L);    /* Wait for unpause. */
        } else {
            for (i = 0; i < c_size[vdt_input]; ++i) {
                lock;

                if (video_devices_running[vdt_input][i] != NULL) {
                    /* Obtain frame image data from device buffers */
                    VideoDevice *device = video_devices_running[vdt_input][i];
                    uint16_t video_width = device->video_width;
                    uint16_t video_height = device->video_height;
                    uint8_t *y = device->input.planes[0];
                    uint8_t *u = device->input.planes[1];
                    uint8_t *v = device->input.planes[2];

#ifndef USE_V4L2

                    if (camera_read(y, u, v, &video_width, &video_height) != 0) {
                        unlock;
                        continue;
                    }

#else /* USE_V4L2 */
                    struct v4l2_buffer buf = {0};

                    buf.type = V4L2_BUF_TYPE_VIDEO_CAPTURE;
                    buf.memory = V4L2_MEMORY_MMAP;

                    if (-1 == ioctl(device->fd, VIDIOC_DQBUF, &buf)) {
                        unlock;
                        continue;
                    }

                    void *data = (void *)device->buffers[buf.index].start;

                    /* Convert frame image data to YUV420 for ToxAV */
                    yuv422to420(y, u, v, data, video_width, video_height);

#endif

                    /* Send frame data to friend through ToxAV */
                    if (device->cb) {
                        device->cb(toxic, device->friend_number, video_width, video_height, y, u, v, device->cb_data);
                    }

                    /* Convert YUV420 data to BGR */
                    uint8_t *img_data = malloc(video_width * video_height * 4);
                    yuv420tobgr(video_width, video_height, y, u, v,
                                video_width, video_width / 2, video_width / 2, img_data);
                    video_window_draw(device, vdt_input, img_data, video_width, video_height);
                    free(img_data);

#ifdef USE_V4L2

                    if (-1 == xioctl(device->fd, VIDIOC_QBUF, &buf)) {
                        unlock;
                        continue;
                    }

#endif

                }

                unlock;
            }

            long int sleep_duration = 1000 * 1000 / 24;
            sleep_thread(sleep_duration);
        }
    }

    pthread_exit(NULL);
}

VideoDeviceError close_video_device(VideoDeviceType type, uint32_t device_idx)
{
    if (device_idx >= MAX_DEVICES) {
        return vde_InvalidSelection;
    }

    lock;
    VideoDevice *device = video_devices_running[type][device_idx];
    VideoDeviceError rc = vde_None;

    if (!device) {
        unlock;
        return vde_DeviceNotActive;
    }

    video_devices_running[type][device_idx] = NULL;

    if (!device->ref_count) {

        if (type == vdt_input) {
#ifndef USE_V4L2

            camera_close(device_idx);
#else /* USE_V4L2 */
            enum v4l2_buf_type buf_type = V4L2_BUF_TYPE_VIDEO_CAPTURE;

            if (-1 == xioctl(device->fd, VIDIOC_STREAMOFF, &buf_type)) {}

            int i;

            for (i = 0; i < device->n_buffers; ++i) {
                if (-1 == munmap(device->buffers[i].start, device->buffers[i].length)) {
                }
            }

            close(device->fd);

#endif
            vpx_img_free(&device->input);
            video_window_close(device, vdt_input);
            pthread_mutex_destroy(device->mutex);

#ifdef USE_V4L2
            free(device->buffers);
#endif /* USE_V4L2 */

            free(device);
        } else {
            vpx_img_free(&device->input);
            video_window_close(device, vdt_output);
            pthread_mutex_destroy(device->mutex);
            free(device);
        }

    } else {
        device->ref_count--;
    }

    unlock;
    return rc;
}

void print_video_devices(ToxWindow *self, const Client_Config *c_config, VideoDeviceType type)
{
    for (int i = 0; i < c_size[type]; ++i) {
        line_info_add(self, c_config, false, NULL, NULL, SYS_MSG, 0, 0, "%d: %s", i, video_devices_names[type][i]);
    }

    return;
}

VideoDeviceError video_selection_valid(VideoDeviceType type, int32_t selection)
{
    return (c_size[type] <= selection || selection < 0) ? vde_InvalidSelection : vde_None;
}

#endif /* VIDEO */
