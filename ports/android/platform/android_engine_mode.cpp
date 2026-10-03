// Which engine the launcher loads next time.
//
// Single player and multiplayer cannot share a process. KISAK_MP is not a
// feature flag: it changes struct layouts (entityState_s, cpose_t, the
// critical section, MAX_CONFIGSTRINGS), so linking both into one binary is an
// ODR violation that manifests as corrupted entity state rather than a link
// error. Retail shipped two executables; the iOS port ships two dylibs and
// dlopens one (ports/ios/app/launcher.mm).
//
// Android does the same thing with two .so files, except that the choice is
// made in Java before any native code runs: the launcher activity calls
// System.loadLibrary("kisakcod_sp") or ("kisakcod_mp"). That is strictly
// better than dlopen from a shim, because the Android loader then resolves
// the library's own dependencies (libvulkan, libaaudio, libmediandk) through
// the APK's lib path without any RPATH games.
//
// The mode therefore has to be readable from both sides. A one-line file in
// private storage is the whole mechanism: the engine writes it when the
// in-game menu switches mode, and the Java launcher reads it on the next
// start. SharedPreferences would need a JNI round trip from an engine thread
// that may not be attached to the VM, for no benefit.

#include "android_platform.h"

#include <cstdio>
#include <cstring>
#include <mutex>
#include <string>

namespace {

std::mutex g_mutex;
char g_mode[8] = "";

std::string ModeFilePath()
{
    const char *root = KisakAndroid_PrivatePath();
    if (!root || !*root)
        return std::string();
    return std::string(root) + "/engine_mode.txt";
}

void LoadMode()
{
    if (g_mode[0])
        return;
    // Multiplayer is the default, matching the iOS port: a fresh install with
    // no game files at all still reaches the server browser, which is where
    // the setup instructions are.
    std::snprintf(g_mode, sizeof(g_mode), "mp");

    const std::string path = ModeFilePath();
    if (path.empty())
        return;
    std::FILE *file = std::fopen(path.c_str(), "rb");
    if (!file)
        return;
    char buffer[8] = {};
    const std::size_t read = std::fread(buffer, 1, sizeof(buffer) - 1, file);
    std::fclose(file);
    buffer[read] = 0;
    for (char &c : buffer)
    {
        if (c == '\n' || c == '\r' || c == ' ')
            c = 0;
    }
    if (std::strcmp(buffer, "sp") == 0)
        std::snprintf(g_mode, sizeof(g_mode), "sp");
}

} // namespace

extern "C" void KisakAndroid_SetEngineMode(const char *mode)
{
    if (!mode)
        return;
    const bool singleplayer = std::strcmp(mode, "sp") == 0;
    std::lock_guard<std::mutex> lock(g_mutex);
    std::snprintf(g_mode, sizeof(g_mode), "%s", singleplayer ? "sp" : "mp");

    const std::string path = ModeFilePath();
    if (path.empty())
        return;
    // Write through a temporary and rename: the player switching mode and
    // then force-killing the app must not leave a truncated file that reads
    // as neither mode.
    const std::string temporary = path + ".tmp";
    std::FILE *file = std::fopen(temporary.c_str(), "wb");
    if (!file)
        return;
    std::fwrite(g_mode, 1, std::strlen(g_mode), file);
    std::fflush(file);
    std::fclose(file);
    std::rename(temporary.c_str(), path.c_str());
}

extern "C" const char *KisakAndroid_GetEngineMode()
{
    std::lock_guard<std::mutex> lock(g_mutex);
    LoadMode();
    return g_mode;
}

extern "C" void KisakAndroid_PromptEngineRestart(const char *mode)
{
    // The engine has already brought up Vulkan, the audio device and its hunk
    // allocator; it cannot tear itself down from inside its own frame loop.
    // Record the choice and tell the player, exactly as the iOS port does.
    KisakAndroid_SetEngineMode(mode);
    const bool singleplayer = mode && std::strcmp(mode, "sp") == 0;
    extern void KisakAndroid_ShowRestartPrompt(const char *title, const char *message);
    KisakAndroid_ShowRestartPrompt(singleplayer ? "Switching to Singleplayer" : "Switching to Multiplayer",
                                   "Close the app and open it again to finish switching.");
}
