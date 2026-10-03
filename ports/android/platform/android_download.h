// HTTP download backend for the multiplayer client (see android_download.cpp).
// Plain C, matching ports/ios/platform/apple_download.h, so src/client_mp sees
// one API on both ports and neither has to pull in the platform's own headers.
#pragma once

#ifdef __cplusplus
extern "C" {
#endif

// Starts one transfer to `localName`; returns 0 if the URL could not be used.
int KisakDownload_Begin(const char *localName, const char *remoteName, int isMotd);
// dlStatus_t: 0 continue, 1 done, 2 failed.
int KisakDownload_Poll(void);
void KisakDownload_Cancel(void);
int KisakDownload_InProgress(void);
int KisakDownload_IsMotd(void);
void KisakDownload_Progress(long long *received, long long *expected);

#ifdef __cplusplus
}
#endif
