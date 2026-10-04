// A crash-surviving text log the player can hand over.
//
// Logcat is not an option for a field report: it needs a cable and adb, it is
// capped at a few hundred kilobytes of ring buffer shared with every other app
// on the device, and on most retail phones the engine's output has already
// scrolled out of it by the time the player notices something went wrong. So
// the port keeps its own file, and the launcher offers it for copying.
//
// Everything here is usable from a signal handler: writes go to a raw file
// descriptor rather than stdio, because a stdio buffer that has not been
// flushed is lost exactly when its contents matter most.

#pragma once

#include <android/log.h>

#include <cstddef>

// Sets the file to write. Must be called before KisakAndroid_LogOpen(); the
// launcher owns the path so that it can rotate and read the file without
// having to agree with native code about where it lives. Falls back to
// KisakAndroid_PrivatePath() when it is never called.
void KisakAndroid_LogSetPath(const char *path);
const char *KisakAndroid_LogPath();

// Opens the file and writes the header. Safe to call more than once.
void KisakAndroid_LogOpen();

void KisakAndroid_LogWrite(const char *text);
void KisakAndroid_LogPrintf(const char *format, ...) __attribute__((format(printf, 1, 2)));

// Mirrors one of the port's own __android_log_print lines into the log file,
// prefixed with its tag and newline-terminated. Without this the renderer's
// diagnostics only ever reach logcat, which a player cannot export - see the
// KISAK_LOG* macros below.
void KisakAndroid_LogTagged(const char *tag, const char *format, ...) __attribute__((format(printf, 2, 3)));
void KisakAndroid_LogFlush();

// Appends the calling thread's stack to the log. bionic has no
// <execinfo.h>, so engine code that would call backtrace() on Apple uses
// this instead.
void KisakAndroid_LogBacktrace();

// Catches the signals that kill a native process, appends the signal, the
// faulting address and a backtrace to the log, then lets the default handler
// run so the system still produces its tombstone.
void KisakAndroid_InstallCrashHandler();

// Log to logcat and to the exportable log file at once. Subsystems define
// their own LOGI/LOGE in terms of these so a bug report carries the renderer,
// video and networking diagnostics too.
#define KISAK_LOGI(tag, ...)                                                                                           \
    do                                                                                                                 \
    {                                                                                                                  \
        __android_log_print(ANDROID_LOG_INFO, (tag), __VA_ARGS__);                                                     \
        KisakAndroid_LogTagged((tag), __VA_ARGS__);                                                                    \
    } while (0)
#define KISAK_LOGE(tag, ...)                                                                                           \
    do                                                                                                                 \
    {                                                                                                                  \
        __android_log_print(ANDROID_LOG_ERROR, (tag), __VA_ARGS__);                                                     \
        KisakAndroid_LogTagged((tag), __VA_ARGS__);                                                                    \
    } while (0)
