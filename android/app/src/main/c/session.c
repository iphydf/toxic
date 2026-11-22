/* SPDX-License-Identifier: GPL-3.0-or-later
 * Copyright © 2026 The TokTok team.
 */

/*
 * Runs toxic in the app's process. toxic's stdin and stdout are a pseudo-terminal, which we read
 * into the terminal emulator (vterm.c) that chat.tox.toxic.Terminal draws. Its stderr goes to
 * logcat.
 */
#define _GNU_SOURCE

#include <AL/alc.h>
#include <android/log.h>
#include <android/native_window_jni.h>
#include <curl/curl.h>
#include <errno.h>
#include <jni.h>
#include <locale.h>
#include <openssl/x509.h>
#include <pthread.h>
#include <pty.h>
#include <signal.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ioctl.h>
#include <unistd.h>

#include "android_video.h"
#include "vterm.h"

#define LOG_TAG "toxic"

/* toxic's own main. */
int main(int argc, char **argv);

static JavaVM *java_vm;
static jclass terminal_class;
static jmethodID on_screen_changed;
static jmethodID on_video_window;
static jmethodID on_call_audio;

/* Its destructor detaches a thread from Java when the thread ends. */
static pthread_key_t attached_key;

static void detach_thread(void *env)
{
    (void)env;
    (*java_vm)->DetachCurrentThread(java_vm);
}

/* Returns the calling thread's JNIEnv, and attaches the thread to Java if it isn't. */
static JNIEnv *thread_env(void)
{
    JNIEnv *env;

    if ((*java_vm)->GetEnv(java_vm, (void **)&env, JNI_VERSION_1_6) == JNI_OK) {
        return env;
    }

    if ((*java_vm)->AttachCurrentThread(java_vm, &env, NULL) != JNI_OK) {
        return NULL;
    }

    pthread_setspecific(attached_key, env);
    return env;
}

/* Protects term: the reader thread writes it and the UI thread draws it. */
static pthread_mutex_t term_lock = PTHREAD_MUTEX_INITIALIZER;
static Vterm *term;
static int master_fd = -1;

/* toxic's curl trusts the certificates in this file, which chat.tox.toxic.Certificates writes. */
static char *ca_file;

/* toxic's command line, from Terminal.start. */
static int toxic_argc;
static char **toxic_argv;

JNIEXPORT jint JNI_OnLoad(JavaVM *vm, void *reserved)
{
    (void)reserved;
    JNIEnv *env;

    if ((*vm)->GetEnv(vm, (void **)&env, JNI_VERSION_1_6) != JNI_OK) {
        return JNI_ERR;
    }

    const jclass cls = (*env)->FindClass(env, "chat/tox/toxic/Terminal");

    if (cls == NULL) {
        return JNI_ERR;
    }

    java_vm = vm;
    terminal_class = (*env)->NewGlobalRef(env, cls);
    on_screen_changed = (*env)->GetStaticMethodID(env, cls, "onScreenChanged", "()V");
    on_video_window = (*env)->GetStaticMethodID(env, cls, "onVideoWindow", "(IZII)V");
    on_call_audio = (*env)->GetStaticMethodID(env, cls, "onCallAudio", "(Z)V");

    if (on_screen_changed == NULL || on_video_window == NULL || on_call_audio == NULL
            || pthread_key_create(&attached_key, detach_thread) != 0) {
        return JNI_ERR;
    }

    return JNI_VERSION_1_6;
}

static void *run_toxic(void *arg)
{
    (void)arg;
    exit(main(toxic_argc, toxic_argv));
}

static void video_window_changed(Android_Video_Window window, bool shown, uint16_t width, uint16_t height)
{
    JNIEnv *env = thread_env();

    if (env != NULL) {
        (*env)->CallStaticVoidMethod(env, terminal_class, on_video_window, (jint)window, (jboolean)shown,
                                     (jint)width, (jint)height);
    }
}

static void call_audio_changed(bool on)
{
    JNIEnv *env = thread_env();

    if (env != NULL) {
        (*env)->CallStaticVoidMethod(env, terminal_class, on_call_audio, (jboolean)on);
    }
}

ALCdevice *__real_alcCaptureOpenDevice(const ALCchar *name, ALCuint frequency, ALCenum format, ALCsizei size);
ALCboolean __real_alcCaptureCloseDevice(ALCdevice *device);

/*
 * build-native.sh links toxic with --wrap for these. toxic only opens the microphone during a call,
 * so we use that to set up the phone's audio for calls.
 */
ALCdevice *__wrap_alcCaptureOpenDevice(const ALCchar *name, ALCuint frequency, ALCenum format, ALCsizei size)
{
    call_audio_changed(true);
    return __real_alcCaptureOpenDevice(name, frequency, format, size);
}

ALCboolean __wrap_alcCaptureCloseDevice(ALCdevice *device)
{
    const ALCboolean closed = __real_alcCaptureCloseDevice(device);
    call_audio_changed(false);
    return closed;
}

CURL *__real_curl_easy_init(void);

/*
 * build-native.sh links toxic with --wrap=curl_easy_init, so its curl handles come from here and
 * trust the device's certificates. Setting SSL_CERT_FILE doesn't work: app processes have
 * AT_SECURE set, so OpenSSL ignores the environment.
 */
CURL *__wrap_curl_easy_init(void)
{
    CURL *curl = __real_curl_easy_init();

    if (curl != NULL && ca_file != NULL) {
        curl_easy_setopt(curl, CURLOPT_CAINFO, ca_file);
    }

    return curl;
}

char *__real_setlocale(int category, const char *locale);

/*
 * build-native.sh links toxic with --wrap=setlocale. Newer Android calls its UTF-8 locale "C", and
 * ncurses then treats it as an 8-bit locale and draws each byte of a character separately.
 */
char *__wrap_setlocale(int category, const char *locale)
{
    char *result = __real_setlocale(category, locale);

    // An 8-bit locale has 1 byte per character.
    if (result != NULL && strcmp(result, "C") == 0 && MB_CUR_MAX > 1) {
        return (char *)"C.UTF-8";
    }

    return result;
}

/* Returns a copy of a Java string, or NULL. */
static char *string_of(JNIEnv *env, jstring string)
{
    const char *chars = (*env)->GetStringUTFChars(env, string, NULL);

    if (chars == NULL) {
        return NULL;
    }

    char *copy = strdup(chars);
    (*env)->ReleaseStringUTFChars(env, string, chars);
    return copy;
}

/* -b enables toxic's stderr, which we send to logcat. */
static const char *const fixed_args[] = {"toxic", "-b"};
#define FIXED_ARGS (int)(sizeof(fixed_args) / sizeof(fixed_args[0]))

/* Makes toxic's argv: fixed_args, then args. Returns -1 if out of memory. */
static int set_args(JNIEnv *env, jobjectArray args)
{
    const jsize count = (*env)->GetArrayLength(env, args);
    const int argc = FIXED_ARGS + (int)count;
    char **argv = calloc((size_t)argc + 1, sizeof(char *));

    if (argv == NULL) {
        return -1;
    }

    for (int i = 0; i < argc; ++i) {
        if (i < FIXED_ARGS) {
            argv[i] = strdup(fixed_args[i]);
        } else {
            const jstring arg = (*env)->GetObjectArrayElement(env, args, i - FIXED_ARGS);
            argv[i] = string_of(env, arg);
            (*env)->DeleteLocalRef(env, arg);
        }

        if (argv[i] == NULL) {
            for (int j = 0; j < i; ++j) {
                free(argv[j]);
            }

            free(argv);
            return -1;
        }
    }

    toxic_argc = argc;
    toxic_argv = argv;
    return 0;
}

static int stderr_fd = -1;

/* Copies toxic's stderr to logcat, a line at a time. */
static void *log_stderr(void *arg)
{
    (void)arg;
    char line[1024];
    size_t length = 0;

    for (;;) {
        char c;
        const ssize_t n = read(stderr_fd, &c, 1);

        if (n < 0 && errno == EINTR) {
            continue;
        }

        if (n <= 0 || c == '\n' || length == sizeof(line) - 1) {
            line[length] = '\0';

            if (length > 0) {
                __android_log_write(ANDROID_LOG_INFO, LOG_TAG, line);
            }

            length = 0;

            if (n <= 0) {
                return NULL;
            }
        }

        if (c != '\n') {
            line[length++] = c;
        }
    }
}

static void *read_terminal(void *arg)
{
    (void)arg;
    JNIEnv *env = thread_env();

    if (env == NULL) {
        return NULL;
    }

    uint8_t buf[16384];

    for (;;) {
        const ssize_t length = read(master_fd, buf, sizeof(buf));

        if (length < 0 && errno == EINTR) {
            continue;
        }

        if (length <= 0) {
            break;
        }

        pthread_mutex_lock(&term_lock);
        vterm_write(term, buf, (size_t)length);
        pthread_mutex_unlock(&term_lock);
        (*env)->CallStaticVoidMethod(env, terminal_class, on_screen_changed);
    }

    __android_log_print(ANDROID_LOG_INFO, LOG_TAG, "terminal closed: %s", strerror(errno));
    return NULL;
}

static int start_thread(void *(*run)(void *))
{
    pthread_t thread;
    const int err = pthread_create(&thread, NULL, run, NULL);

    if (err == 0) {
        pthread_detach(thread);
    }

    return err;
}

JNIEXPORT jboolean JNICALL Java_chat_tox_toxic_Terminal_start(JNIEnv *env, jclass cls, jstring home, jstring cache,
        jobjectArray args, jint rows, jint cols)
{
    (void)cls;

    if (term != NULL) {
        return JNI_TRUE;
    }

    Vterm *new_term = vterm_new(rows, cols);
    char *home_dir = string_of(env, home);
    char *cache_dir = string_of(env, cache);
    char *config_dir = NULL;
    struct winsize size = {(unsigned short)rows, (unsigned short)cols, 0, 0};
    int slave_fd;

    if (new_term == NULL || home_dir == NULL || cache_dir == NULL || set_args(env, args) != 0
            || asprintf(&config_dir, "%s/.config", home_dir) < 0
            || asprintf(&ca_file, "%s/cacerts.pem", cache_dir) < 0
            || openpty(&master_fd, &slave_fd, NULL, NULL, &size) != 0) {
        __android_log_print(ANDROID_LOG_ERROR, LOG_TAG, "can't start toxic: %s", strerror(errno));
        vterm_free(new_term);
        free(home_dir);
        free(cache_dir);
        free(config_dir);
        return JNI_FALSE;
    }

    term = new_term;
    android_video_set_listener(video_window_changed);
    setenv("HOME", home_dir, 1);
    setenv("XDG_CONFIG_HOME", config_dir, 1);
    setenv("TMPDIR", cache_dir, 1);
    setenv("TERM", "toxic-android", 1);
    setenv("LANG", "C.UTF-8", 1);

    // Received files go to the working directory.
    if (chdir(home_dir) != 0) {
        __android_log_print(ANDROID_LOG_WARN, LOG_TAG, "chdir: %s", strerror(errno));
    }

    free(home_dir);
    free(cache_dir);
    free(config_dir);

    int stderr_pipe[2];

    if (pipe(stderr_pipe) != 0) {
        __android_log_print(ANDROID_LOG_ERROR, LOG_TAG, "pipe: %s", strerror(errno));
        return JNI_FALSE;
    }

    stderr_fd = stderr_pipe[0];
    dup2(slave_fd, STDIN_FILENO);
    dup2(slave_fd, STDOUT_FILENO);
    dup2(stderr_pipe[1], STDERR_FILENO);
    close(stderr_pipe[1]);

    if (slave_fd > STDERR_FILENO) {
        close(slave_fd);
    }

    return start_thread(read_terminal) == 0 && start_thread(log_stderr) == 0 && start_thread(run_toxic) == 0
           ? JNI_TRUE : JNI_FALSE;
}

JNIEXPORT void JNICALL Java_chat_tox_toxic_Terminal_write(JNIEnv *env, jclass cls, jbyteArray data)
{
    (void)cls;
    const jsize length = (*env)->GetArrayLength(env, data);
    jbyte *bytes = (*env)->GetByteArrayElements(env, data, NULL);

    for (jsize done = 0; bytes != NULL && done < length;) {
        const ssize_t n = write(master_fd, bytes + done, (size_t)(length - done));

        if (n < 0 && errno != EINTR) {
            break;
        }

        done += n > 0 ? (jsize)n : 0;
    }

    (*env)->ReleaseByteArrayElements(env, data, bytes, JNI_ABORT);
}

JNIEXPORT void JNICALL Java_chat_tox_toxic_Terminal_resize(JNIEnv *env, jclass cls, jint rows, jint cols)
{
    (void)env;
    (void)cls;

    if (term == NULL) {
        return;
    }

    pthread_mutex_lock(&term_lock);
    const int err = vterm_resize(term, rows, cols);
    pthread_mutex_unlock(&term_lock);

    if (err == 0) {
        const struct winsize size = {(unsigned short)rows, (unsigned short)cols, 0, 0};
        ioctl(master_fd, TIOCSWINSZ, &size);
        // The pty isn't our controlling terminal, so the kernel doesn't send SIGWINCH itself.
        kill(getpid(), SIGWINCH);
    }
}

/* Copies the screen into chars (VTERM_CELL_CHARS per cell) and looks (fg, bg and style per cell,
 * as in Vterm_Look), row by row. Cells outside the emulator's size are blank. */
JNIEXPORT void JNICALL Java_chat_tox_toxic_Terminal_snapshot(JNIEnv *env, jclass cls, jint rows, jint cols,
        jintArray chars, jintArray looks)
{
    (void)cls;

    if (term == NULL || (*env)->GetArrayLength(env, chars) < rows * cols * VTERM_CELL_CHARS
            || (*env)->GetArrayLength(env, looks) < rows * cols * 3) {
        return;
    }

    jint *out_chars = (*env)->GetIntArrayElements(env, chars, NULL);
    jint *out_looks = (*env)->GetIntArrayElements(env, looks, NULL);

    if (out_chars != NULL && out_looks != NULL) {
        const Vterm_Look blank = {VTERM_DEFAULT_FG_RGB, VTERM_DEFAULT_BG_RGB, 0};
        pthread_mutex_lock(&term_lock);

        for (int row = 0; row < rows; ++row) {
            for (int col = 0; col < cols; ++col) {
                const int i = row * cols + col;
                const bool inside = row < vterm_rows(term) && col < vterm_cols(term);
                const uint32_t *cell = inside ? vterm_chars(term, row, col) : NULL;
                const Vterm_Look look = inside ? vterm_look(term, row, col) : blank;

                for (int j = 0; j < VTERM_CELL_CHARS; ++j) {
                    out_chars[i * VTERM_CELL_CHARS + j] = cell != NULL ? (jint)cell[j] : j == 0 ? ' ' : 0;
                }

                out_looks[i * 3] = (jint)look.fg;
                out_looks[i * 3 + 1] = (jint)look.bg;
                out_looks[i * 3 + 2] = (jint)look.style;
            }
        }

        pthread_mutex_unlock(&term_lock);
    }

    if (out_chars != NULL) {
        (*env)->ReleaseIntArrayElements(env, chars, out_chars, 0);
    }

    if (out_looks != NULL) {
        (*env)->ReleaseIntArrayElements(env, looks, out_looks, 0);
    }
}

/* Whether OpenSSL can read a DER certificate. OpenSSL ignores a whole CA file if it can't read
 * one of its certificates. */
JNIEXPORT jboolean JNICALL Java_chat_tox_toxic_Terminal_readsCertificate(JNIEnv *env, jclass cls, jbyteArray der)
{
    (void)cls;
    const jsize length = (*env)->GetArrayLength(env, der);
    jbyte *bytes = (*env)->GetByteArrayElements(env, der, NULL);

    if (bytes == NULL) {
        return JNI_FALSE;
    }

    const unsigned char *p = (const unsigned char *)bytes;
    X509 *cert = d2i_X509(NULL, &p, length);
    const jboolean ok = cert != NULL && p == (const unsigned char *)bytes + length;
    X509_free(cert);
    (*env)->ReleaseByteArrayElements(env, der, bytes, JNI_ABORT);
    return ok;
}

/*
 * toxic ends the process with exit() (linked with --wrap=exit). exit() runs every library's static
 * destructors while the app's own threads still use them, and Android aborts the first thread that
 * locks a destroyed mutex. toxic has already saved and cleaned up, so flush stdio and _exit
 * instead.
 */
_Noreturn void __wrap_exit(int status);

_Noreturn void __wrap_exit(int status)
{
    fflush(NULL);
    _exit(status);
}

/* Asks toxic to save and exit, as pressing Ctrl+C twice within a second does. raise() runs
 * toxic's handler on this thread before it returns, so the 2 signals don't merge into 1. */
JNIEXPORT void JNICALL Java_chat_tox_toxic_Terminal_quit(JNIEnv *env, jclass cls)
{
    (void)env;
    (void)cls;
    raise(SIGINT);
    raise(SIGINT);
}

/* Sets the surface toxic draws a video window (Android_Video_Window) on, or null. */
JNIEXPORT void JNICALL Java_chat_tox_toxic_Terminal_setVideoSurface(JNIEnv *env, jclass cls, jint window,
        jobject surface)
{
    (void)cls;

    if (window >= 0 && window < ANDROID_VIDEO_WINDOWS) {
        android_video_set_surface((Android_Video_Window)window,
                                  surface != NULL ? ANativeWindow_fromSurface(env, surface) : NULL);
    }
}
