/*  android_video.c
 *
 *  Copyright (C) 2026 Toxic All Rights Reserved.
 *
 *  This file is part of Toxic. Toxic is free software licensed
 *  under the GNU General Public License 3.0.
 */

#include "android_video.h"

#ifdef __ANDROID__

#include <android/log.h>
#include <android/native_window.h>
#include <camera/NdkCameraCaptureSession.h>
#include <camera/NdkCameraDevice.h>
#include <camera/NdkCameraManager.h>
#include <camera/NdkCameraMetadata.h>
#include <media/NdkImage.h>
#include <media/NdkImageReader.h>
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define LOG_TAG "toxic"
#define MAX_CAMERAS 8

/* We ask the camera for this size, or the closest it has. */
#define WANTED_WIDTH 640
#define WANTED_HEIGHT 480

typedef struct Camera {
    char *id;
    bool front;
    /* Degrees clockwise to turn the camera's pictures upright, with the phone held upright. */
    int32_t orientation;
} Camera;

static ACameraManager *manager;
static Camera cameras[MAX_CAMERAS];
static int camera_count;

/* The open camera, if any. */
static Camera *camera;
static ACameraDevice *device;
static AImageReader *reader;
static ANativeWindow *reader_window;
static ACaptureSessionOutputContainer *outputs;
static ACaptureSessionOutput *output;
static ACameraOutputTarget *target;
static ACaptureRequest *request;
static ACameraCaptureSession *session;
static int32_t capture_width;
static int32_t capture_height;

static pthread_mutex_t window_lock = PTHREAD_MUTEX_INITIALIZER;
static Android_Video_Listener *listener;
static ANativeWindow *surfaces[ANDROID_VIDEO_WINDOWS];
static uint16_t window_width[ANDROID_VIDEO_WINDOWS];
static uint16_t window_height[ANDROID_VIDEO_WINDOWS];

int android_video_init(char **device_names, int *size)
{
    *size = 0;
    manager = ACameraManager_create();

    if (manager == NULL) {
        return -1;
    }

    ACameraIdList *ids;

    if (ACameraManager_getCameraIdList(manager, &ids) != ACAMERA_OK) {
        return -1;
    }

    for (int i = 0; i < ids->numCameras && camera_count < MAX_CAMERAS; ++i) {
        ACameraMetadata *metadata;

        if (ACameraManager_getCameraCharacteristics(manager, ids->cameraIds[i], &metadata) != ACAMERA_OK) {
            continue;
        }

        ACameraMetadata_const_entry facing = {0};
        ACameraMetadata_const_entry orientation = {0};
        ACameraMetadata_getConstEntry(metadata, ACAMERA_LENS_FACING, &facing);
        ACameraMetadata_getConstEntry(metadata, ACAMERA_SENSOR_ORIENTATION, &orientation);

        Camera *c = &cameras[camera_count];
        c->id = strdup(ids->cameraIds[i]);
        c->front = facing.count > 0 && facing.data.u8[0] == ACAMERA_LENS_FACING_FRONT;
        c->orientation = orientation.count > 0 ? orientation.data.i32[0] : 0;
        ACameraMetadata_free(metadata);

        const char *kind = c->front ? "Front camera"
                           : facing.count > 0 && facing.data.u8[0] == ACAMERA_LENS_FACING_BACK ? "Back camera"
                           : "Camera";
        char name[64];
        snprintf(name, sizeof(name), "%s (%s)", kind, c->id);
        device_names[camera_count] = strdup(name);

        if (c->id == NULL || device_names[camera_count] == NULL) {
            free(c->id);
            free(device_names[camera_count]);
            continue;
        }

        ++camera_count;
    }

    ACameraManager_deleteCameraIdList(ids);
    *size = camera_count;
    return 0;
}

void android_video_release(void)
{
    for (int i = 0; i < camera_count; ++i) {
        free(cameras[i].id);
    }

    camera_count = 0;

    if (manager != NULL) {
        ACameraManager_delete(manager);
        manager = NULL;
    }
}

/* Picks the camera's YUV size closest to the one we want. */
static void choose_size(const char *id, int32_t *width, int32_t *height)
{
    *width = WANTED_WIDTH;
    *height = WANTED_HEIGHT;
    ACameraMetadata *metadata;

    if (ACameraManager_getCameraCharacteristics(manager, id, &metadata) != ACAMERA_OK) {
        return;
    }

    ACameraMetadata_const_entry configs = {0};
    ACameraMetadata_getConstEntry(metadata, ACAMERA_SCALER_AVAILABLE_STREAM_CONFIGURATIONS, &configs);
    long best = -1;

    /* Each configuration is format, width, height and whether it's an input. */
    for (uint32_t i = 0; i + 3 < configs.count; i += 4) {
        const int32_t *config = &configs.data.i32[i];

        if (config[0] != AIMAGE_FORMAT_YUV_420_888
                || config[3] != ACAMERA_SCALER_AVAILABLE_STREAM_CONFIGURATIONS_OUTPUT) {
            continue;
        }

        const long distance = labs((long)config[1] * config[2] - (long)WANTED_WIDTH * WANTED_HEIGHT);

        if (best < 0 || distance < best) {
            best = distance;
            *width = config[1];
            *height = config[2];
        }
    }

    ACameraMetadata_free(metadata);
}

static void on_disconnected(void *context, ACameraDevice *dev)
{
    (void)context;
    (void)dev;
    __android_log_write(ANDROID_LOG_WARN, LOG_TAG, "camera disconnected");
}

static void on_error(void *context, ACameraDevice *dev, int error)
{
    (void)context;
    (void)dev;
    __android_log_print(ANDROID_LOG_WARN, LOG_TAG, "camera error %d", error);
}

void android_video_close_device(uint32_t device_idx)
{
    (void)device_idx;

    if (session != NULL) {
        ACameraCaptureSession_stopRepeating(session);
        ACameraCaptureSession_close(session);
        session = NULL;
    }

    if (request != NULL) {
        ACaptureRequest_free(request);
        request = NULL;
    }

    if (target != NULL) {
        ACameraOutputTarget_free(target);
        target = NULL;
    }

    if (outputs != NULL) {
        if (output != NULL) {
            ACaptureSessionOutputContainer_remove(outputs, output);
        }

        ACaptureSessionOutputContainer_free(outputs);
        outputs = NULL;
    }

    if (output != NULL) {
        ACaptureSessionOutput_free(output);
        output = NULL;
    }

    if (device != NULL) {
        ACameraDevice_close(device);
        device = NULL;
    }

    if (reader != NULL) {
        AImageReader_delete(reader);
        reader = NULL;
        reader_window = NULL;
    }

    camera = NULL;
}

int android_video_open_device(uint32_t selection, uint16_t *width, uint16_t *height)
{
    if (selection >= (uint32_t)camera_count || camera != NULL) {
        return -1;
    }

    Camera *c = &cameras[selection];
    choose_size(c->id, &capture_width, &capture_height);

    ACameraDevice_StateCallbacks device_callbacks = {NULL, on_disconnected, on_error};
    ACameraCaptureSession_stateCallbacks session_callbacks = {0};

    if (AImageReader_new(capture_width, capture_height, AIMAGE_FORMAT_YUV_420_888, 2, &reader) != AMEDIA_OK
            || AImageReader_getWindow(reader, &reader_window) != AMEDIA_OK
            || ACameraManager_openCamera(manager, c->id, &device_callbacks, &device) != ACAMERA_OK
            || ACaptureSessionOutputContainer_create(&outputs) != ACAMERA_OK
            || ACaptureSessionOutput_create(reader_window, &output) != ACAMERA_OK
            || ACaptureSessionOutputContainer_add(outputs, output) != ACAMERA_OK
            || ACameraDevice_createCaptureSession(device, outputs, &session_callbacks, &session) != ACAMERA_OK
            || ACameraDevice_createCaptureRequest(device, TEMPLATE_RECORD, &request) != ACAMERA_OK
            || ACameraOutputTarget_create(reader_window, &target) != ACAMERA_OK
            || ACaptureRequest_addTarget(request, target) != ACAMERA_OK
            || ACameraCaptureSession_setRepeatingRequest(session, NULL, 1, &request, NULL) != ACAMERA_OK) {
        __android_log_print(ANDROID_LOG_WARN, LOG_TAG, "can't start camera %s", c->id);
        android_video_close_device(selection);
        return -1;
    }

    camera = c;
    const bool turned = c->orientation == 90 || c->orientation == 270;
    *width = (uint16_t)(turned ? capture_height : capture_width);
    *height = (uint16_t)(turned ? capture_width : capture_height);
    return 0;
}

/* Finds where the pixel at x, y of the upright picture is in the camera's picture (w by h). */
static void source_of(int32_t orientation, int x, int y, int w, int h, int *sx, int *sy)
{
    switch (orientation) {
        case 90:
            *sx = y;
            *sy = h - 1 - x;
            break;

        case 180:
            *sx = w - 1 - x;
            *sy = h - 1 - y;
            break;

        case 270:
            *sx = w - 1 - y;
            *sy = x;
            break;

        default:
            *sx = x;
            *sy = y;
            break;
    }
}

/* Copies one plane of an image upright into out, which is out_w by out_h with no padding. */
static void copy_plane(const AImage *image, int plane, int32_t orientation, int src_w, int src_h,
                       uint8_t *out, int out_w, int out_h)
{
    uint8_t *data;
    int length;
    int32_t row_stride;
    int32_t pixel_stride;

    if (AImage_getPlaneData(image, plane, &data, &length) != AMEDIA_OK
            || AImage_getPlaneRowStride(image, plane, &row_stride) != AMEDIA_OK
            || AImage_getPlanePixelStride(image, plane, &pixel_stride) != AMEDIA_OK) {
        return;
    }

    for (int y = 0; y < out_h; ++y) {
        for (int x = 0; x < out_w; ++x) {
            int sx;
            int sy;
            source_of(orientation, x, y, src_w, src_h, &sx, &sy);
            const long offset = (long)sy * row_stride + (long)sx * pixel_stride;
            out[y * out_w + x] = offset < length ? data[offset] : 0;
        }
    }
}

int android_video_read_device(uint8_t *y, uint8_t *u, uint8_t *v, uint16_t *width, uint16_t *height)
{
    AImage *image;

    if (reader == NULL || camera == NULL || AImageReader_acquireLatestImage(reader, &image) != AMEDIA_OK) {
        return -1;
    }

    const int32_t orientation = camera->orientation;
    const bool turned = orientation == 90 || orientation == 270;
    const int out_w = turned ? capture_height : capture_width;
    const int out_h = turned ? capture_width : capture_height;

    copy_plane(image, 0, orientation, capture_width, capture_height, y, out_w, out_h);
    copy_plane(image, 1, orientation, capture_width / 2, capture_height / 2, u, out_w / 2, out_h / 2);
    copy_plane(image, 2, orientation, capture_width / 2, capture_height / 2, v, out_w / 2, out_h / 2);
    AImage_delete(image);

    *width = (uint16_t)out_w;
    *height = (uint16_t)out_h;
    return 0;
}

void android_video_set_listener(Android_Video_Listener *new_listener)
{
    pthread_mutex_lock(&window_lock);
    listener = new_listener;
    pthread_mutex_unlock(&window_lock);
}

void android_video_set_surface(Android_Video_Window window, ANativeWindow *surface)
{
    pthread_mutex_lock(&window_lock);

    if (surfaces[window] != NULL) {
        ANativeWindow_release(surfaces[window]);
    }

    surfaces[window] = surface;
    pthread_mutex_unlock(&window_lock);
}

void android_video_window_open(Android_Video_Window window)
{
    pthread_mutex_lock(&window_lock);
    window_width[window] = 0;
    window_height[window] = 0;

    if (listener != NULL) {
        listener(window, true, 0, 0);
    }

    pthread_mutex_unlock(&window_lock);
}

void android_video_window_close(Android_Video_Window window)
{
    pthread_mutex_lock(&window_lock);

    if (listener != NULL) {
        listener(window, false, 0, 0);
    }

    pthread_mutex_unlock(&window_lock);
}

void android_video_window_draw(Android_Video_Window window, const uint8_t *bgra, uint16_t width, uint16_t height)
{
    pthread_mutex_lock(&window_lock);

    if (width != window_width[window] || height != window_height[window]) {
        window_width[window] = width;
        window_height[window] = height;

        if (listener != NULL) {
            listener(window, true, width, height);
        }
    }

    ANativeWindow *surface = surfaces[window];
    ANativeWindow_Buffer buffer;

    if (surface == NULL
            || ANativeWindow_setBuffersGeometry(surface, width, height, WINDOW_FORMAT_RGBA_8888) != 0
            || ANativeWindow_lock(surface, &buffer, NULL) != 0) {
        pthread_mutex_unlock(&window_lock);
        return;
    }

    /* Our own front camera looks like a mirror. */
    const bool mirror = window == ANDROID_VIDEO_PREVIEW && camera != NULL && camera->front;

    for (int y = 0; y < height && y < buffer.height; ++y) {
        uint8_t *row = (uint8_t *)buffer.bits + (size_t)y * buffer.stride * 4;

        for (int x = 0; x < width && x < buffer.width; ++x) {
            const uint8_t *pixel = bgra + ((size_t)y * width + (mirror ? width - 1 - x : x)) * 4;
            row[x * 4 + 0] = pixel[2];
            row[x * 4 + 1] = pixel[1];
            row[x * 4 + 2] = pixel[0];
            row[x * 4 + 3] = 0xff;
        }
    }

    ANativeWindow_unlockAndPost(surface);
    pthread_mutex_unlock(&window_lock);
}

#endif /* __ANDROID__ */
