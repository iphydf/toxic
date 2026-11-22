/*  android_video.h
 *
 *  Copyright (C) 2026 Toxic All Rights Reserved.
 *
 *  This file is part of Toxic. Toxic is free software licensed
 *  under the GNU General Public License 3.0.
 */

/*
 * Video on Android. Cameras come from the NDK, like osx_video.h does for macOS. The app provides
 * the windows that video_device.c draws in.
 */
#ifndef ANDROID_VIDEO_H
#define ANDROID_VIDEO_H

#include <stdbool.h>
#include <stdint.h>

struct ANativeWindow;

int android_video_init(char **device_names, int *size);
void android_video_release(void);
int android_video_open_device(uint32_t selection, uint16_t *width, uint16_t *height);
void android_video_close_device(uint32_t device_idx);
int android_video_read_device(uint8_t *y, uint8_t *u, uint8_t *v, uint16_t *width, uint16_t *height);

typedef enum Android_Video_Window {
    ANDROID_VIDEO_PREVIEW,  /* Our camera. */
    ANDROID_VIDEO_RECEIVE,  /* The friend's video. */
} Android_Video_Window;

#define ANDROID_VIDEO_WINDOWS 2

void android_video_window_open(Android_Video_Window window);
void android_video_window_close(Android_Video_Window window);

/* bgra is width * height pixels of 4 bytes each, blue first (as video_device.c makes them). */
void android_video_window_draw(Android_Video_Window window, const uint8_t *bgra, uint16_t width, uint16_t height);

/*
 * For the app. The listener is told when a window opens (shown, with the video's size once known)
 * or closes. android_video_set_surface takes over the caller's reference to the surface, which can
 * be NULL.
 */
typedef void Android_Video_Listener(Android_Video_Window window, bool shown, uint16_t width, uint16_t height);
void android_video_set_listener(Android_Video_Listener *listener);
void android_video_set_surface(Android_Video_Window window, struct ANativeWindow *surface);

#endif /* ANDROID_VIDEO_H */
