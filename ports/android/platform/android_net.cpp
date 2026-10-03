// UDP networking for the Android multiplayer build, replacing src/win32/win_net.cpp.
//
// Single player never opens a socket, so this only matters for KISAK_MP. The
// engine hands packets to Sys_SendPacket and polls Sys_GetPacket; both the
// client and a listen server run over the bound sockets here.
//
// Differences from the iOS version (ports/ios/platform/apple_net.cpp), all of
// them real rather than stylistic:
//
//   * bionic's sockaddr_in has no sin_len field.
//   * SO_NOSIGPIPE does not exist on Linux; MSG_NOSIGNAL per-send is the
//     equivalent, and UDP sends cannot raise SIGPIPE anyway.
//   * Android's multi-network support means "the default route" is not
//     stable: a phone on Wi-Fi and mobile data at once has two, and a socket
//     bound before the switch keeps sending into the dead one. The Java layer
//     watches ConnectivityManager and calls KisakAndroid_NetworkChanged(),
//     which rebinds.
//   * Link-local and CGNAT ranges have to count as LAN for the server
//     browser, because carrier-grade NAT (100.64/10) is extremely common on
//     mobile and a player behind it is not on the public internet.

#include <universal/q_shared.h>
#include <qcommon/qcommon.h>
#include <qcommon/net_chan_mp.h>

#include <arpa/inet.h>
#include <errno.h>
#include <fcntl.h>
#include <ifaddrs.h>
#include <net/if.h>
#include <netdb.h>
#include <netinet/in.h>
#include <sys/socket.h>
#include <unistd.h>

#include <atomic>
#include <cstring>

namespace {

int s_ipSocket = -1;
int s_serverSocket = -1;
std::atomic<bool> s_networkChanged{false};
uint16_t s_clientPort = 0;
uint16_t s_serverPort = 0;

void NetadrToSockadr(const netadr_t &address, sockaddr_in &out)
{
    memset(&out, 0, sizeof(out));
    out.sin_family = AF_INET;
    if (address.type == NA_BROADCAST)
        out.sin_addr.s_addr = INADDR_BROADCAST;
    else
        memcpy(&out.sin_addr.s_addr, address.ip, 4);
    out.sin_port = address.port; // already network order, as in the original
}

void SockadrToNetadr(const sockaddr_in &address, netadr_t &out)
{
    memset(&out, 0, sizeof(out));
    out.type = NA_IP;
    memcpy(out.ip, &address.sin_addr.s_addr, 4);
    out.port = address.sin_port;
}

int OpenSocket(uint16_t port, const char *label)
{
    const int handle = socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
    if (handle < 0)
    {
        Com_PrintError(CON_CHANNEL_SYSTEM, "NET: socket() failed for %s: %s\n", label, strerror(errno));
        return -1;
    }

    const int flags = fcntl(handle, F_GETFL, 0);
    if (flags < 0 || fcntl(handle, F_SETFL, flags | O_NONBLOCK) < 0)
    {
        Com_PrintError(CON_CHANNEL_SYSTEM, "NET: cannot set %s non-blocking: %s\n", label, strerror(errno));
        close(handle);
        return -1;
    }

    const int yes = 1;
    setsockopt(handle, SOL_SOCKET, SO_BROADCAST, &yes, sizeof(yes));
    // The engine sends bursts at cl_maxpackets; the default Android send
    // buffer is small enough that a burst can return EAGAIN mid-snapshot.
    const int bufferBytes = 128 * 1024;
    setsockopt(handle, SOL_SOCKET, SO_SNDBUF, &bufferBytes, sizeof(bufferBytes));
    setsockopt(handle, SOL_SOCKET, SO_RCVBUF, &bufferBytes, sizeof(bufferBytes));
    // Mark the traffic as interactive so Wi-Fi drivers with WMM put it in the
    // video access category rather than best effort. Worth 10-20 ms of
    // latency on a congested access point, and free when the driver ignores it.
    const int tos = 0xB8; // DSCP EF
    setsockopt(handle, IPPROTO_IP, IP_TOS, &tos, sizeof(tos));

    sockaddr_in address{};
    address.sin_family = AF_INET;
    address.sin_addr.s_addr = INADDR_ANY;
    address.sin_port = htons(port);
    if (bind(handle, reinterpret_cast<sockaddr *>(&address), sizeof(address)) < 0)
    {
        Com_PrintError(CON_CHANNEL_SYSTEM, "NET: cannot bind %s to port %u: %s\n", label, port, strerror(errno));
        close(handle);
        return -1;
    }
    return handle;
}

void CloseSocket(int &handle)
{
    if (handle >= 0)
    {
        close(handle);
        handle = -1;
    }
}

} // namespace

// Called from the Java connectivity callback when the active network changes.
extern "C" void KisakAndroid_NetworkChanged()
{
    s_networkChanged.store(true, std::memory_order_release);
}

void Sys_InitNetworking(uint16_t clientPort, uint16_t serverPort)
{
    s_clientPort = clientPort;
    s_serverPort = serverPort;
    s_ipSocket = OpenSocket(clientPort, "client");
    s_serverSocket = OpenSocket(serverPort, "server");
}

void Sys_ShutdownNetworking()
{
    CloseSocket(s_ipSocket);
    CloseSocket(s_serverSocket);
}

int __cdecl Sys_GetPacket(netadr_t *net_from, msg_t *net_message)
{
    // A network switch leaves the old sockets bound to an interface that no
    // longer routes. Rebinding here, on the engine's own thread, avoids
    // tearing sockets down underneath a send on another one.
    if (s_networkChanged.exchange(false, std::memory_order_acq_rel))
    {
        Com_Printf(CON_CHANNEL_SYSTEM, "NET: active network changed, rebinding sockets\n");
        CloseSocket(s_ipSocket);
        CloseSocket(s_serverSocket);
        s_ipSocket = OpenSocket(s_clientPort, "client");
        s_serverSocket = OpenSocket(s_serverPort, "server");
    }

    const int sockets[2] = { s_ipSocket, s_serverSocket };
    for (const int handle : sockets)
    {
        if (handle < 0)
            continue;
        sockaddr_in from{};
        socklen_t fromLength = sizeof(from);
        const ssize_t received = recvfrom(handle, net_message->data, static_cast<size_t>(net_message->maxsize), 0,
                                          reinterpret_cast<sockaddr *>(&from), &fromLength);
        if (received < 0)
        {
            if (errno == EAGAIN || errno == EWOULDBLOCK || errno == EINTR)
                continue;
            // ECONNREFUSED arrives on UDP when a previous send got an ICMP
            // port-unreachable back. It describes a packet already gone, not
            // this socket, so it must not be treated as a failure.
            if (errno == ECONNREFUSED || errno == EHOSTUNREACH || errno == ENETUNREACH)
                continue;
            Com_PrintError(CON_CHANNEL_SYSTEM, "NET: recvfrom failed: %s\n", strerror(errno));
            continue;
        }
        if (received == 0)
            continue;
        if (received >= net_message->maxsize)
        {
            Com_Printf(CON_CHANNEL_SYSTEM, "NET: oversize packet discarded\n");
            continue;
        }
        SockadrToNetadr(from, *net_from);
        net_message->readcount = 0;
        net_message->cursize = static_cast<int>(received);
        return 1;
    }
    return 0;
}

char __cdecl Sys_SendPacket(int length, unsigned char *data, netadr_t to)
{
    if (to.type == NA_LOOPBACK)
        return 1; // handled by the engine's loopback path
    const int handle = to.type == NA_BROADCAST || s_ipSocket < 0 ? s_serverSocket : s_ipSocket;
    if (handle < 0)
        return 0;

    sockaddr_in address{};
    NetadrToSockadr(to, address);
    const ssize_t sent = sendto(handle, data, static_cast<size_t>(length), MSG_NOSIGNAL,
                                reinterpret_cast<sockaddr *>(&address), sizeof(address));
    if (sent == length)
        return 1;
    if (sent < 0)
    {
        // A full send buffer is normal on a congested mobile link; the engine
        // resends. Anything else is worth a line in the console.
        if (errno == EAGAIN || errno == EWOULDBLOCK || errno == EINTR)
            return 0;
        Com_PrintError(CON_CHANNEL_SYSTEM, "NET: sendto failed: %s\n", strerror(errno));
    }
    return 0;
}

bool __cdecl Sys_StringToAdr(const char *text, netadr_t *address)
{
    memset(address, 0, sizeof(*address));
    if (!text || !*text)
        return false;
    if (!I_stricmp(text, "localhost"))
    {
        address->type = NA_LOOPBACK;
        return true;
    }

    char host[256];
    I_strncpyz(host, text, sizeof(host));
    const char *service = nullptr;
    // Split host:port, keeping the engine's behaviour of leaving the port at
    // zero when none is given so the caller can apply its own default.
    char *colon = strchr(host, ':');
    if (colon)
    {
        *colon = 0;
        service = colon + 1;
    }

    addrinfo hints{};
    hints.ai_family = AF_INET;
    hints.ai_socktype = SOCK_DGRAM;
    addrinfo *results = nullptr;
    // getaddrinfo on Android resolves through the active network's DNS; a
    // numeric address still works when there is no network at all.
    if (getaddrinfo(host, service, &hints, &results) != 0 || !results)
        return false;

    bool resolved = false;
    for (addrinfo *entry = results; entry; entry = entry->ai_next)
    {
        if (entry->ai_family != AF_INET)
            continue;
        SockadrToNetadr(*reinterpret_cast<sockaddr_in *>(entry->ai_addr), *address);
        resolved = true;
        break;
    }
    freeaddrinfo(results);
    return resolved;
}

void Sys_ShowIP()
{
    ifaddrs *interfaces = nullptr;
    if (getifaddrs(&interfaces) != 0)
    {
        Com_PrintError(CON_CHANNEL_SYSTEM, "NET: cannot enumerate interfaces: %s\n", strerror(errno));
        return;
    }
    for (ifaddrs *entry = interfaces; entry; entry = entry->ifa_next)
    {
        if (!entry->ifa_addr || entry->ifa_addr->sa_family != AF_INET)
            continue;
        if (!(entry->ifa_flags & IFF_UP) || (entry->ifa_flags & IFF_LOOPBACK))
            continue;
        char text[INET_ADDRSTRLEN] = "";
        const sockaddr_in *address = reinterpret_cast<const sockaddr_in *>(entry->ifa_addr);
        if (inet_ntop(AF_INET, &address->sin_addr, text, sizeof(text)))
            Com_Printf(CON_CHANNEL_SYSTEM, "IP: %s (%s)\n", text, entry->ifa_name ? entry->ifa_name : "?");
    }
    freeifaddrs(interfaces);
}

bool Sys_IsLANAddress(netadr_t adr)
{
    if (adr.type == NA_LOOPBACK)
        return true;
    if (adr.type != NA_IP && adr.type != NA_BROADCAST)
        return false;
    const unsigned char *ip = adr.ip;
    if (ip[0] == 10 || ip[0] == 127)
        return true;
    if (ip[0] == 192 && ip[1] == 168)
        return true;
    if (ip[0] == 172 && ip[1] >= 16 && ip[1] <= 31)
        return true;
    // 169.254/16 link-local: Wi-Fi Direct and USB tethering land here.
    if (ip[0] == 169 && ip[1] == 254)
        return true;
    // 100.64/10 carrier-grade NAT. Most mobile networks put handsets in this
    // range, so treating it as public would mark normal players as remote.
    if (ip[0] == 100 && ip[1] >= 64 && ip[1] <= 127)
        return true;
    return false;
}

bool Sys_IsLANAddress_IgnoreSubnet(netadr_t adr)
{
    return Sys_IsLANAddress(adr);
}

void NET_Sleep(int msec)
{
    if (msec > 0)
        Sys_Sleep(static_cast<unsigned int>(msec));
}

// ---------------------------------------------------------------------------
// Voice chat. CoD4 runs Speex over the same channel; no capture device is
// wired up in this port, so these stay stubs, as they are on iOS.

void Voice_Init() {}
void Voice_Shutdown() {}
void Voice_Playback() {}
double __cdecl Voice_GetVoiceLevel() { return 0.0; }
int __cdecl Voice_GetLocalVoiceData() { return 0; }
bool __cdecl Voice_IsClientTalking(uint32_t clientNum)
{
    (void)clientNum;
    return false;
}
void __cdecl Voice_IncomingVoiceData(unsigned char clientNum, unsigned char *data, int dataSize)
{
    (void)clientNum;
    (void)data;
    (void)dataSize;
}

// ---------------------------------------------------------------------------
// Console and process control, which Android does not offer a sandboxed app.

void __cdecl Sys_ShowConsole() {}

void __cdecl Sys_QuitAndStartProcess(const char *exeName, const char *parameters)
{
    // Android has no exec for an app's own package, and the SP/MP split is
    // handled by reloading a different .so on the next launch instead.
    (void)parameters;
    Com_Printf(CON_CHANNEL_SYSTEM, "Cannot start '%s': restart the app to switch engines\n", exeName ? exeName : "");
}

void IN_SetCursorPos(POINT position)
{
    (void)position;
}
