// Where the port keeps its files.
//
// Android gives an app three useful locations and the NDK can derive none of
// them: they come from Context on the Java side and are handed down once at
// startup. The port uses them as follows.
//
//   gameData  - getExternalFilesDir(null), i.e.
//               /sdcard/Android/data/<package>/files. Visible over MTP and in
//               any file manager, so the player can copy main/ and zone/ into
//               it from a PC without root or special permissions, and it is
//               deleted with the app so no orphaned 10 GB folder is left
//               behind. This is the equivalent of the iOS Documents folder.
//   private   - getFilesDir(): profiles, saves, the console log. Internal
//               storage, so writes are fast and not subject to the scoped
//               storage rules that make external writes slow.
//   cache     - getCacheDir(): the pipeline/shader cache and converted
//               cinematics. The platform may delete it under memory pressure,
//               which is correct for data that can be regenerated.
//
// Paths are copied into fixed buffers rather than std::string because the
// engine reads them from signal-unsafe contexts (the crash handler logs the
// game data path) where an allocation would be a second fault.

#include "android_platform.h"

#include <sys/stat.h>
#include <sys/types.h>

#include <cstdio>
#include <cstring>
#include <mutex>

namespace {

constexpr std::size_t kPathMax = 1024;

char g_gameData[kPathMax] = "";
char g_private[kPathMax] = "";
char g_cache[kPathMax] = "";
std::once_flag g_logged;

void CopyPath(char *destination, const char *source)
{
    if (!source || !*source)
    {
        destination[0] = 0;
        return;
    }
    std::snprintf(destination, kPathMax, "%s", source);
    // Trailing separators turn "root" + "/main" into "root//main", which most
    // of the engine tolerates and the fastfile loader does not.
    std::size_t length = std::strlen(destination);
    while (length > 1 && destination[length - 1] == '/')
        destination[--length] = 0;
}

void EnsureDirectory(const char *path)
{
    if (!path || !*path)
        return;
    // Only the leaf: Java already created the parents by asking for them.
    mkdir(path, 0770);
}

} // namespace

void KisakAndroid_SetStorageRoots(const char *gameData, const char *internalCache, const char *externalCache)
{
    CopyPath(g_gameData, gameData);
    CopyPath(g_private, internalCache);
    CopyPath(g_cache, externalCache);

    EnsureDirectory(g_gameData);
    EnsureDirectory(g_private);
    EnsureDirectory(g_cache);

    std::call_once(g_logged, [] {
        // Goes to logcat rather than the engine console: this runs before
        // Com_Init, so Com_Printf is not available yet, and a wrong path here
        // is the single most common cause of a "game files not found" report.
        std::fprintf(stderr, "KisakCOD: game data '%s' private '%s' cache '%s'\n", g_gameData, g_private, g_cache);
    });
}

const char *KisakAndroid_GameDataPath()
{
    return g_gameData;
}

const char *KisakAndroid_PrivatePath()
{
    // Falling back to the game data folder keeps saves working on a device
    // where the Java side failed to report internal storage, rather than
    // silently writing to the current directory, which on Android is "/".
    return g_private[0] ? g_private : g_gameData;
}

const char *KisakAndroid_CachePath()
{
    return g_cache[0] ? g_cache : KisakAndroid_PrivatePath();
}
