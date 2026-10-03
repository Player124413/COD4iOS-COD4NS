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
void KisakAndroid_LogFlush();

// Catches the signals that kill a native process, appends the signal, the
// faulting address and a backtrace to the log, then lets the default handler
// run so the system still produces its tombstone.
void KisakAndroid_InstallCrashHandler();
