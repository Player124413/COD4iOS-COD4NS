// Master server query.
//
// Mirrors ports/ios/platform/apple_master.cpp, including the header it
// presents to the engine (android_master.h is a copy of apple_master.h). The
// only differences are bionic's: SO_NOSIGPIPE does not exist, so writes use
// MSG_NOSIGNAL instead.
//
// The query runs on its own thread because it happens while the server
// browser is on screen and the engine is still rendering; a blocking connect
// to an unreachable master would otherwise freeze the UI for the TCP timeout.

#include "android_master.h"

#include <android/log.h>

#include <atomic>
#include <cerrno>
#include <cstring>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

#include <arpa/inet.h>
#include <fcntl.h>
#include <netdb.h>
#include <netinet/in.h>
#include <netinet/tcp.h>
#include <poll.h>
#include <sys/socket.h>
#include <unistd.h>

#define LOGI(...) __android_log_print(ANDROID_LOG_INFO, "KisakCOD-master", __VA_ARGS__)
#define LOGE(...) __android_log_print(ANDROID_LOG_ERROR, "KisakCOD-master", __VA_ARGS__)

namespace {

constexpr const char *kMasterHost = "cod4master.cod4x.ovh";
constexpr const char *kMasterPort = "20810";

// The CoD4X master speaks the Quake 3 out-of-band protocol over TCP. The
// four 0xFF bytes are the out-of-band marker; 21 is the protocol version.
constexpr char kRequest[] = "\xff\xff\xff\xffgetservers 21 full empty";
constexpr std::size_t kRequestLength = sizeof(kRequest) - 1;

// A master with tens of thousands of servers would still fit; the cap exists
// so a hostile or broken master cannot make the app allocate without bound.
constexpr std::size_t kResponseCap = 256 * 1024;
constexpr int kConnectTimeoutMs = 5000;
constexpr int kTotalDeadlineMs = 10000;

std::mutex g_mutex;
std::vector<KisakMasterAddress> g_addresses;
std::atomic<int> g_state{ 0 }; // 0 idle/pending, 1 done, -1 failed
std::atomic<bool> g_running{ false };

int64_t NowMs()
{
    struct timespec ts{};
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return static_cast<int64_t>(ts.tv_sec) * 1000 + ts.tv_nsec / 1000000;
}

bool SetNonBlocking(int fd)
{
    const int flags = fcntl(fd, F_GETFL, 0);
    return flags >= 0 && fcntl(fd, F_SETFL, flags | O_NONBLOCK) == 0;
}

// Connects with a bounded wait. getaddrinfo may return several records (the
// master is dual-homed); each is tried in turn until one connects.
int ConnectWithTimeout(int64_t deadline)
{
    struct addrinfo hints{};
    hints.ai_family = AF_INET; // the reply format is IPv4-only
    hints.ai_socktype = SOCK_STREAM;

    struct addrinfo *results = nullptr;
    const int error = getaddrinfo(kMasterHost, kMasterPort, &hints, &results);
    if (error != 0 || !results)
    {
        LOGE("getaddrinfo(%s): %s", kMasterHost, gai_strerror(error));
        return -1;
    }

    int connected = -1;
    for (struct addrinfo *entry = results; entry && connected < 0; entry = entry->ai_next)
    {
        const int fd = socket(entry->ai_family, entry->ai_socktype, entry->ai_protocol);
        if (fd < 0)
            continue;
        if (!SetNonBlocking(fd))
        {
            close(fd);
            continue;
        }

        // Nagle would hold the single small request back waiting for more.
        const int one = 1;
        setsockopt(fd, IPPROTO_TCP, TCP_NODELAY, &one, sizeof(one));

        if (connect(fd, entry->ai_addr, entry->ai_addrlen) == 0)
        {
            connected = fd;
            break;
        }
        if (errno != EINPROGRESS)
        {
            close(fd);
            continue;
        }

        const int64_t remaining = std::min<int64_t>(kConnectTimeoutMs, deadline - NowMs());
        if (remaining <= 0)
        {
            close(fd);
            break;
        }

        struct pollfd poller{};
        poller.fd = fd;
        poller.events = POLLOUT;
        const int ready = poll(&poller, 1, static_cast<int>(remaining));
        if (ready <= 0)
        {
            close(fd);
            continue;
        }

        // poll reporting writable does not mean the connect succeeded; the
        // socket error has to be read back explicitly.
        int socketError = 0;
        socklen_t length = sizeof(socketError);
        if (getsockopt(fd, SOL_SOCKET, SO_ERROR, &socketError, &length) < 0 || socketError != 0)
        {
            close(fd);
            continue;
        }
        connected = fd;
    }

    freeaddrinfo(results);
    return connected;
}

// The reply is a stream of "\\" followed by four address bytes and two port
// bytes, terminated by "EOT". Parsing is deliberately tolerant: a truncated
// trailing record is dropped rather than failing the whole query.
void ParseResponse(const std::vector<uint8_t> &data, std::vector<KisakMasterAddress> &out)
{
    std::size_t i = 0;
    // Skip the out-of-band header and the "getserversResponse" token.
    while (i + 1 < data.size() && !(data[i] == '\\'))
        ++i;

    while (i + 6 < data.size())
    {
        if (data[i] != '\\')
        {
            ++i;
            continue;
        }
        if (std::memcmp(&data[i + 1], "EOT", 3) == 0)
            break;

        KisakMasterAddress address{};
        std::memcpy(address.ip, &data[i + 1], 4);
        std::memcpy(address.port, &data[i + 5], 2);
        i += 7;

        // 0.0.0.0 and port 0 are padding some masters emit.
        const bool blankIp = address.ip[0] == 0 && address.ip[1] == 0 && address.ip[2] == 0 && address.ip[3] == 0;
        const bool blankPort = address.port[0] == 0 && address.port[1] == 0;
        if (blankIp || blankPort)
            continue;
        out.push_back(address);
    }
}

void QueryThread()
{
    const int64_t deadline = NowMs() + kTotalDeadlineMs;
    std::vector<KisakMasterAddress> addresses;

    const int fd = ConnectWithTimeout(deadline);
    if (fd < 0)
    {
        LOGE("could not reach the master server");
        g_state.store(-1);
        g_running.store(false);
        return;
    }

    // MSG_NOSIGNAL in place of SO_NOSIGPIPE: bionic has no such socket
    // option, and without one of the two a master that closes the connection
    // early kills the process with SIGPIPE.
    std::size_t sent = 0;
    bool ok = true;
    while (sent < kRequestLength && ok)
    {
        const ssize_t written = send(fd, kRequest + sent, kRequestLength - sent, MSG_NOSIGNAL);
        if (written > 0)
        {
            sent += static_cast<std::size_t>(written);
            continue;
        }
        if (written < 0 && (errno == EAGAIN || errno == EWOULDBLOCK))
        {
            struct pollfd poller{ fd, POLLOUT, 0 };
            const int64_t remaining = deadline - NowMs();
            if (remaining <= 0 || poll(&poller, 1, static_cast<int>(remaining)) <= 0)
                ok = false;
            continue;
        }
        ok = false;
    }

    std::vector<uint8_t> response;
    while (ok)
    {
        const int64_t remaining = deadline - NowMs();
        if (remaining <= 0)
            break;

        struct pollfd poller{ fd, POLLIN, 0 };
        const int ready = poll(&poller, 1, static_cast<int>(remaining));
        if (ready < 0)
        {
            if (errno == EINTR)
                continue;
            ok = false;
            break;
        }
        if (ready == 0)
            break; // deadline; whatever arrived is what we parse

        uint8_t chunk[8192];
        const ssize_t received = recv(fd, chunk, sizeof(chunk), 0);
        if (received == 0)
            break; // the master closed, which is how the reply ends
        if (received < 0)
        {
            if (errno == EAGAIN || errno == EWOULDBLOCK || errno == EINTR)
                continue;
            ok = false;
            break;
        }
        if (response.size() + static_cast<std::size_t>(received) > kResponseCap)
        {
            LOGE("master reply exceeded %zu bytes; truncating", kResponseCap);
            response.insert(response.end(), chunk, chunk + (kResponseCap - response.size()));
            break;
        }
        response.insert(response.end(), chunk, chunk + received);
    }

    close(fd);

    if (!response.empty())
        ParseResponse(response, addresses);

    {
        std::lock_guard<std::mutex> lock(g_mutex);
        g_addresses = std::move(addresses);
        LOGI("master returned %zu servers", g_addresses.size());
    }
    g_state.store(1);
    g_running.store(false);
}

} // namespace

void KisakMaster_Begin()
{
    bool expected = false;
    if (!g_running.compare_exchange_strong(expected, true))
        return; // a query is already in flight

    {
        std::lock_guard<std::mutex> lock(g_mutex);
        g_addresses.clear();
    }
    g_state.store(0);

    // Detached: the engine polls for the result and never joins. The thread
    // has a hard deadline, so it cannot outlive the activity by more than
    // kTotalDeadlineMs.
    std::thread(QueryThread).detach();
}

int KisakMaster_Poll(KisakMasterAddress *addresses, int capacity)
{
    const int state = g_state.load();
    if (state == 0)
        return 0;
    if (state < 0)
        return -1;

    std::lock_guard<std::mutex> lock(g_mutex);
    const int count = std::min<int>(capacity, static_cast<int>(g_addresses.size()));
    if (addresses && count > 0)
        std::memcpy(addresses, g_addresses.data(), static_cast<std::size_t>(count) * sizeof(KisakMasterAddress));
    return count;
}
