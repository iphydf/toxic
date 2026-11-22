# Options for Android, built by android/build-native.sh with UNAME_S=Android.

# Video comes from the cameras and is shown by the app, not X11 (android_video.c).
VIDEO_LIBS = openal vpx
OBJ += android_video.o
LDFLAGS += -lcamera2ndk -lmediandk -landroid -llog
