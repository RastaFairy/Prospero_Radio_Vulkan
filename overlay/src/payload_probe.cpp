// Prospero Radio - local payload persistence bridge.
// Copyright (C) 2026 BlackBearReloaded
// SPDX-License-Identifier: GPL-3.0-or-later

#include "payload_probe.hpp"

#if defined(PROSPERO_DATA_BRIDGE_ENABLED)

#include <arpa/inet.h>
#include <netinet/in.h>
#include <sys/socket.h>
#include <sys/time.h>

#include <atomic>
#include <cerrno>
#include <cstddef>
#include <cstdarg>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <ctime>
#include <mutex>
#include <unistd.h>

extern "C"
{
    int sceKernelUsleep(std::uint32_t microseconds);
}

namespace
{

constexpr char kPayloadPath[] = "/app0/assets/payload/ProsperoRadioDataBridge.elf";
constexpr char kReportPath[] = "/download0/prospero-payload-probe.log";
constexpr char kSharedReportPath[] = "/download0/prospero-payload-shared.log";
constexpr char kPayloadFilename[] = "ProsperoRadioDataBridge.elf";
constexpr std::uint32_t kMaximumTransfer = 64U * 1024U * 1024U;
constexpr std::size_t kTransferChunk = 32768U;
constexpr char kBridgeMagic[] = "PRPC";
constexpr unsigned short kBridgePort = 7001;

enum BridgeOp : unsigned char
{
    kPing = 1,
    kGet = 2,
    kPut = 3,
    kStop = 4,
    kStatus = 5,
    kStartAux = 6,
    kStopAux = 7,
    kAttach = 8
};

enum BridgeStatus : unsigned char
{
    kBridgeOk = 0,
    kBridgeNotFound = 1,
    kBridgeBadRequest = 2,
    kBridgeIoError = 3
};

struct FileSpec
{
    const char *path;
};

std::atomic<bool> g_bridge_ready{false};
std::atomic<bool> g_report_file_unavailable{false};
std::mutex g_bridge_mutex;
int g_bridge_socket = -1;

FileSpec GetFileSpec(radio_payload_file_t file)
{
    switch (file)
    {
    case RADIO_PAYLOAD_CATALOG:
        return {"/download0/radio-browser.sqlite3"};
    case RADIO_PAYLOAD_FAVORITES:
        return {"/download0/radio-browser-favorites.bin"};
    case RADIO_PAYLOAD_EQ:
        return {"/download0/radio-eq.txt"};
    case RADIO_PAYLOAD_PRESETS:
        return {"/download0/radio-presets.bin"};
    case RADIO_PAYLOAD_REPORT:
        return {kSharedReportPath};
    case RADIO_PAYLOAD_AUXLIST:
        return {"/download0/radio-aux.m3u"};
    case RADIO_PAYLOAD_AUXFAVORITES:
        return {"/download0/radio-aux-favorites.bin"};
    default:
        return {nullptr};
    }
}

void Report(const char *format, ...)
{
    char message[384];
    va_list args;
    va_start(args, format);
    const int length = std::vsnprintf(message, sizeof(message), format, args);
    va_end(args);
    if (length < 0)
        return;
    const std::size_t message_length = static_cast<std::size_t>(length) < sizeof(message)
                                           ? static_cast<std::size_t>(length)
                                           : sizeof(message) - 1U;
    std::fprintf(stderr, "[ProsperoRadioDataBridge.elf] %.*s\n",
                 static_cast<int>(message_length), message);
    if (!g_report_file_unavailable.load())
    {
        std::FILE *file = std::fopen(kReportPath, "ab");
        if (file != nullptr)
        {
            std::fprintf(file, "[APP] %.*s\n", static_cast<int>(message_length), message);
            std::fclose(file);
        }
        else
            g_report_file_unavailable.store(true);
    }
}

bool SendAll(int socket_fd, const void *data, std::size_t size)
{
    const auto *bytes = static_cast<const unsigned char *>(data);
    while (size != 0)
    {
        const ssize_t sent = send(socket_fd, bytes, size, 0);
        if (sent < 0 && errno == EINTR)
            continue;
        if (sent <= 0)
            return false;
        bytes += sent;
        size -= static_cast<std::size_t>(sent);
    }
    return true;
}

bool ReceiveAll(int socket_fd, void *data, std::size_t size)
{
    auto *bytes = static_cast<unsigned char *>(data);
    while (size != 0)
    {
        const ssize_t received = recv(socket_fd, bytes, size, 0);
        if (received < 0 && errno == EINTR)
            continue;
        if (received <= 0)
        {
            if (received == 0)
                errno = ECONNRESET;
            return false;
        }
        bytes += received;
        size -= static_cast<std::size_t>(received);
    }
    return true;
}

void SetNoSigPipe(int socket_fd)
{
#ifdef SO_NOSIGPIPE
    const int enabled = 1;
    (void)setsockopt(socket_fd, SOL_SOCKET, SO_NOSIGPIPE, &enabled, sizeof(enabled));
#else
    (void)socket_fd;
#endif
}

int BridgeChannel()
{
    if (g_bridge_socket < 0)
    {
        errno = ENOTCONN;
        return -1;
    }
    return g_bridge_socket;
}

void CloseBridgeChannel()
{
    if (g_bridge_socket >= 0)
    {
        close(g_bridge_socket);
        g_bridge_socket = -1;
    }
    g_bridge_ready.store(false);
}

void WriteU32Be(unsigned char *data, std::uint32_t value)
{
    data[0] = static_cast<unsigned char>(value >> 24);
    data[1] = static_cast<unsigned char>(value >> 16);
    data[2] = static_cast<unsigned char>(value >> 8);
    data[3] = static_cast<unsigned char>(value);
}

std::uint32_t ReadU32Be(const unsigned char *data)
{
    return (static_cast<std::uint32_t>(data[0]) << 24) |
           (static_cast<std::uint32_t>(data[1]) << 16) |
           (static_cast<std::uint32_t>(data[2]) << 8) | static_cast<std::uint32_t>(data[3]);
}

bool SendRequestHeader(int socket_fd, BridgeOp operation, unsigned char file_id,
                       std::uint32_t size)
{
    unsigned char header[12] = {'P', 'R', 'P', 'C', 1, static_cast<unsigned char>(operation),
                                file_id, 0, 0, 0, 0, 0};
    WriteU32Be(header + 8, size);
    return SendAll(socket_fd, header, sizeof(header));
}

bool ReceiveResponseHeader(int socket_fd, BridgeStatus *status, std::uint32_t *size)
{
    unsigned char header[12]{};
    if (!ReceiveAll(socket_fd, header, sizeof(header)))
        return false;
    /* sizeof(kBridgeMagic) includes the C string's trailing NUL. The wire
     * magic is exactly four bytes; byte 4 is the protocol version. */
    if (std::memcmp(header, kBridgeMagic, sizeof(kBridgeMagic) - 1U) != 0 ||
        header[4] != 1 || header[6] != 0 || header[7] != 0)
    {
        errno = EPROTO;
        return false;
    }
    *status = static_cast<BridgeStatus>(header[5]);
    *size = ReadU32Be(header + 8);
    return true;
}

bool PingBridge()
{
    const int socket_fd = BridgeChannel();
    if (socket_fd < 0)
        return false;
    BridgeStatus status{};
    std::uint32_t response_size = 0;
    const bool ok = SendRequestHeader(socket_fd, kPing, 0, 0) &&
                    ReceiveResponseHeader(socket_fd, &status, &response_size) &&
                    status == kBridgeOk && response_size == 0;
    if (!ok)
        CloseBridgeChannel();
    return ok;
}

bool ConnectBridgeChannel(unsigned attempts)
{
    sockaddr_in address{};
    address.sin_len = sizeof(address);
    address.sin_family = AF_INET;
    address.sin_port = htons(kBridgePort);
    address.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    int last_error = ECONNREFUSED;
    for (unsigned attempt = 0; attempt < attempts; ++attempt)
    {
        const int candidate = socket(AF_INET, SOCK_STREAM, 0);
        if (candidate < 0)
        {
            last_error = errno;
            break;
        }
        SetNoSigPipe(candidate);
        const timeval timeout{10, 0};
        (void)setsockopt(candidate, SOL_SOCKET, SO_RCVTIMEO, &timeout, sizeof(timeout));
        (void)setsockopt(candidate, SOL_SOCKET, SO_SNDTIMEO, &timeout, sizeof(timeout));
        if (connect(candidate, reinterpret_cast<const sockaddr *>(&address), sizeof(address)) == 0)
        {
            g_bridge_socket = candidate;
            return true;
        }
        last_error = errno;
        close(candidate);
        if (last_error != ECONNREFUSED && last_error != EINTR && last_error != ETIMEDOUT)
            break;
        if (attempt + 1U < attempts)
            (void)sceKernelUsleep(100000);
    }
    errno = last_error;
    return false;
}

bool AttachCurrentProcess()
{
    const long process_id = static_cast<long>(getpid());
    if (process_id <= 1 || process_id > 0x7fffffffL)
        return false;
    const int socket_fd = BridgeChannel();
    BridgeStatus status{};
    std::uint32_t response_size = 0;
    const bool ok = SendRequestHeader(socket_fd, kAttach, 0,
                                      static_cast<std::uint32_t>(process_id)) &&
                    ReceiveResponseHeader(socket_fd, &status, &response_size) &&
                    status == kBridgeOk && response_size == 0;
    if (!ok)
        CloseBridgeChannel();
    return ok;
}

bool CheckPayloadHealthBounded()
{
    if (!PingBridge())
    {
        Report("payload RPC handshake FAIL errno=%d", errno);
        return false;
    }
    Report("payload RPC handshake PASS on loopback port %u", kBridgePort);
    g_bridge_ready.store(true);
    return true;
}

bool WaitForPayloadExit(int socket_fd)
{
    /* Once the payload exits, the RPC connection closes and the app observes EOF. */
    const timeval timeout{3, 0};
    (void)setsockopt(socket_fd, SOL_SOCKET, SO_RCVTIMEO, &timeout, sizeof(timeout));
    unsigned char drain[256];
    std::size_t drained = 0;
    for (;;)
    {
        ssize_t received;
        do
        {
            received = recv(socket_fd, drain, sizeof(drain), 0);
        } while (received < 0 && errno == EINTR);
        if (received == 0)
            return true;
        if (received < 0)
            return false;
        drained += static_cast<std::size_t>(received);
        if (drained > 4096U)
            return false;
    }
}

bool RequestNamedElfFromElfldr()
{
    sockaddr_in address{};
    address.sin_len = sizeof(address);
    address.sin_family = AF_INET;
    address.sin_port = htons(9021);
    address.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    int connect_error = 0;
    int socket_fd = -1;
    bool connected = false;
    for (unsigned attempt = 0; attempt < 10; ++attempt)
    {
        const int candidate = socket(AF_INET, SOCK_STREAM, 0);
        if (candidate < 0)
        {
            connect_error = errno;
            break;
        }
        SetNoSigPipe(candidate);
        const timeval timeout{10, 0};
        (void)setsockopt(candidate, SOL_SOCKET, SO_RCVTIMEO, &timeout, sizeof(timeout));
        (void)setsockopt(candidate, SOL_SOCKET, SO_SNDTIMEO, &timeout, sizeof(timeout));
        if (connect(candidate, reinterpret_cast<const sockaddr *>(&address), sizeof(address)) == 0)
        {
            socket_fd = candidate;
            connected = true;
            break;
        }
        connect_error = errno;
        close(candidate);
        if (connect_error != ECONNREFUSED && connect_error != EINTR && connect_error != ETIMEDOUT)
            break;
        if (attempt + 1U < 10U)
            (void)sceKernelUsleep(100000);
    }
    if (!connected)
    {
        close(socket_fd);
        Report("elfldr connect 127.0.0.1:9021 FAIL errno=%d after bounded retries", connect_error);
        return false;
    }

    char uri[320];
    const int uri_size = std::snprintf(
        uri, sizeof(uri),
        "file:/system_ex/app/PPSA99001/assets/payload/%s?args=%ld&pipe=0\n",
        kPayloadFilename, static_cast<long>(getpid()));
    if (uri_size <= 0 || static_cast<std::size_t>(uri_size) >= sizeof(uri))
    {
        close(socket_fd);
        Report("elfldr URI formatting FAIL");
        return false;
    }
    Report("elfldr file URI submit name=%s uri=%s", kPayloadFilename, uri);
    const bool sent = SendAll(socket_fd, uri, static_cast<std::size_t>(uri_size));
    const int send_error = errno;
    if (!sent)
    {
        close(socket_fd);
        Report("elfldr URI send FAIL errno=%d", send_error);
        return false;
    }
    /* pipe=0 keeps elfldr's control socket separate; the app connects to the
     * payload-owned loopback RPC listener after elfldr accepts the URI. */
    close(socket_fd);
    Report("elfldr URI sent; waiting for payload RPC on 127.0.0.1:%u", kBridgePort);
    return true;
}

bool GetRemoteFile(radio_payload_file_t file, const char *local_path, bool *not_found)
{
    if (not_found)
        *not_found = false;
    if (local_path == nullptr)
        return false;
    const int socket_fd = BridgeChannel();
    if (socket_fd < 0)
        return false;
    BridgeStatus status{};
    std::uint32_t remaining = 0;
    const bool request_sent = SendRequestHeader(socket_fd, kGet,
                                                static_cast<unsigned char>(file), 0);
    if (!request_sent || !ReceiveResponseHeader(socket_fd, &status, &remaining))
    {
        CloseBridgeChannel();
        return false;
    }
    if (status == kBridgeNotFound)
    {
        if (not_found)
            *not_found = true;
        return false;
    }
    if (status != kBridgeOk)
        return false;
    if (remaining > kMaximumTransfer)
    {
        CloseBridgeChannel();
        return false;
    }

    char temporary[512];
    const int path_length = std::snprintf(temporary, sizeof(temporary), "%s.bridge-tmp", local_path);
    if (path_length <= 0 || static_cast<std::size_t>(path_length) >= sizeof(temporary))
    {
        CloseBridgeChannel();
        return false;
    }
    std::FILE *output = std::fopen(temporary, "wb");
    if (output == nullptr)
    {
        CloseBridgeChannel();
        return false;
    }
    unsigned char buffer[kTransferChunk];
    bool ok = true;
    const std::uint32_t total = remaining;
    while (remaining > 0)
    {
        const std::size_t chunk = remaining < sizeof(buffer) ? remaining : sizeof(buffer);
        if (!ReceiveAll(socket_fd, buffer, chunk) ||
            std::fwrite(buffer, 1, chunk, output) != chunk)
        {
            ok = false;
            break;
        }
        remaining -= static_cast<std::uint32_t>(chunk);
    }
    if (std::fclose(output) != 0)
        ok = false;
    if (!ok)
        CloseBridgeChannel();
    if (ok && std::rename(temporary, local_path) != 0)
        ok = false;
    if (!ok)
        std::remove(temporary);
    if (ok)
        Report("restore file id=%u bytes=%u PASS", static_cast<unsigned>(file), total);
    return ok;
}

bool PutLocalFile(radio_payload_file_t file, const char *local_path)
{
    if (local_path == nullptr)
        return false;
    std::FILE *input = std::fopen(local_path, "rb");
    if (input == nullptr)
        return false;
    if (std::fseek(input, 0, SEEK_END) != 0)
    {
        std::fclose(input);
        return false;
    }
    const long file_size = std::ftell(input);
    if (file_size < 0 || static_cast<unsigned long>(file_size) > kMaximumTransfer ||
        std::fseek(input, 0, SEEK_SET) != 0)
    {
        std::fclose(input);
        return false;
    }
    const int socket_fd = BridgeChannel();
    if (socket_fd < 0)
    {
        std::fclose(input);
        return false;
    }
    bool ok = SendRequestHeader(socket_fd, kPut, static_cast<unsigned char>(file),
                                static_cast<std::uint32_t>(file_size));
    unsigned char buffer[kTransferChunk];
    long remaining = file_size;
    while (ok && remaining > 0)
    {
        const std::size_t chunk = static_cast<std::size_t>(remaining) < sizeof(buffer)
                                      ? static_cast<std::size_t>(remaining)
                                      : sizeof(buffer);
        if (std::fread(buffer, 1, chunk, input) != chunk || !SendAll(socket_fd, buffer, chunk))
        {
            ok = false;
            break;
        }
        remaining -= static_cast<long>(chunk);
    }
    std::fclose(input);
    BridgeStatus status{};
    std::uint32_t response_size = 0;
    if (!ok)
        CloseBridgeChannel();
    if (ok)
    {
        if (!ReceiveResponseHeader(socket_fd, &status, &response_size))
        {
            CloseBridgeChannel();
            ok = false;
        }
        else
            ok = status == kBridgeOk && response_size == 0;
    }
    if (ok)
        Report("persist file id=%u bytes=%ld PASS", static_cast<unsigned>(file), file_size);
    return ok;
}

bool SyncFile(radio_payload_file_t file)
{
    const FileSpec spec = GetFileSpec(file);
    if (!g_bridge_ready.load() || spec.path == nullptr || file == RADIO_PAYLOAD_REPORT ||
        file == RADIO_PAYLOAD_AUXLIST)
        return false;
    bool not_found = false;
    if (GetRemoteFile(file, spec.path, &not_found))
        return true;
    if (not_found)
    {
        if (PutLocalFile(file, spec.path))
        {
            Report("persistent file id=%u seeded from local cache", static_cast<unsigned>(file));
            return true;
        }
        Report("persistent file id=%u absent; no local seed", static_cast<unsigned>(file));
    }
    else
        Report("persistent file id=%u restore failed; retaining local cache",
               static_cast<unsigned>(file));
    return false;
}

bool PushFile(radio_payload_file_t file)
{
    const FileSpec spec = GetFileSpec(file);
    if (!g_bridge_ready.load() || spec.path == nullptr || file == RADIO_PAYLOAD_REPORT)
        return false;
    const bool result = PutLocalFile(file, spec.path);
    if (!result)
        Report("persist file id=%u FAIL errno=%d", static_cast<unsigned>(file), errno);
    return result;
}

bool FetchPayloadReport()
{
    if (!g_bridge_ready.load())
        return false;
    bool not_found = false;
    if (!GetRemoteFile(RADIO_PAYLOAD_REPORT, kSharedReportPath, &not_found))
    {
        Report("shared payload report fetch %s", not_found ? "NOT_FOUND" : "FAIL");
        return false;
    }
    Report("shared payload report copied to %s", kSharedReportPath);
    return true;
}

bool SendSimpleCommand(BridgeOp operation)
{
    const int socket_fd = BridgeChannel();
    if (socket_fd < 0)
        return false;
    BridgeStatus status{};
    std::uint32_t response_size = 0;
    if (!SendRequestHeader(socket_fd, operation, 0, 0) ||
        !ReceiveResponseHeader(socket_fd, &status, &response_size))
    {
        CloseBridgeChannel();
        return false;
    }
    const bool ok = status == kBridgeOk && response_size == 0;
    if (!ok && (status == kBridgeBadRequest || status == kBridgeIoError))
        Report("RPC command %u rejected status=%u", static_cast<unsigned>(operation),
               static_cast<unsigned>(status));
    if (status == kBridgeBadRequest || response_size != 0)
    {
        CloseBridgeChannel();
        return false;
    }
    return ok;
}

bool QueryAuxState(bool *running)
{
    if (running == nullptr || !g_bridge_ready.load())
        return false;
    const int socket_fd = BridgeChannel();
    BridgeStatus status{};
    std::uint32_t response_size = 0;
    if (socket_fd < 0 || !SendRequestHeader(socket_fd, kStatus, 0, 0) ||
        !ReceiveResponseHeader(socket_fd, &status, &response_size))
    {
        if (socket_fd >= 0)
            CloseBridgeChannel();
        return false;
    }
    if (status != kBridgeOk || response_size != 1U)
    {
        Report("AUX status RPC invalid status=%u size=%u", static_cast<unsigned>(status),
               response_size);
        CloseBridgeChannel();
        return false;
    }
    unsigned char flags = 0;
    if (!ReceiveAll(socket_fd, &flags, sizeof(flags)))
    {
        CloseBridgeChannel();
        return false;
    }
    *running = (flags & 1U) != 0;
    return true;
}

} // namespace

#endif /* PROSPERO_DATA_BRIDGE_ENABLED */

bool radio_payload_bridge_start()
{
#if defined(PROSPERO_DATA_BRIDGE_ENABLED)
    std::lock_guard<std::mutex> lock(g_bridge_mutex);
    if (g_bridge_ready.load() && PingBridge())
        return true;
    CloseBridgeChannel();
    Report("ProsperoRadioDataBridge.elf startup begin epoch=%lld",
           static_cast<long long>(std::time(nullptr)));
    /* Reattach to a helper left alive by a fast app restart before launching
     * another ELF instance. The payload changes ownership to this PID. */
    if (ConnectBridgeChannel(1U) && CheckPayloadHealthBounded() && AttachCurrentProcess())
    {
        Report("existing payload bridge attached to app pid=%ld", static_cast<long>(getpid()));
        return true;
    }
    CloseBridgeChannel();
    std::FILE *payload = std::fopen(kPayloadPath, "rb");
    if (payload == nullptr)
    {
        Report("payload open FAIL errno=%d", errno);
        return false;
    }
    if (std::fseek(payload, 0, SEEK_END) != 0)
    {
        Report("payload seek-end FAIL errno=%d", errno);
        std::fclose(payload);
        return false;
    }
    const long payload_size = std::ftell(payload);
    const bool valid_size = payload_size > 0 &&
                            static_cast<unsigned long>(payload_size) <= 1024U * 1024U &&
                            std::fseek(payload, 0, SEEK_SET) == 0;
    std::fclose(payload);
    if (!valid_size)
    {
        Report("payload size/rewind FAIL size=%ld errno=%d", payload_size, errno);
        return false;
    }
    Report("payload asset opened bytes=%ld", payload_size);
    if (!RequestNamedElfFromElfldr() || !ConnectBridgeChannel(40U) ||
        !CheckPayloadHealthBounded() || !AttachCurrentProcess())
    {
        CloseBridgeChannel();
        return false;
    }
    Report("payload bridge owned by app pid=%ld", static_cast<long>(getpid()));
    return true;
#else
    return false;
#endif
}

bool radio_payload_bridge_sync(radio_payload_file_t file)
{
#if defined(PROSPERO_DATA_BRIDGE_ENABLED)
    std::lock_guard<std::mutex> lock(g_bridge_mutex);
    return SyncFile(file);
#else
    (void)file;
    return false;
#endif
}

bool radio_payload_bridge_push(radio_payload_file_t file)
{
#if defined(PROSPERO_DATA_BRIDGE_ENABLED)
    std::lock_guard<std::mutex> lock(g_bridge_mutex);
    return PushFile(file);
#else
    (void)file;
    return false;
#endif
}

bool radio_payload_bridge_push_file(radio_payload_file_t file, const char *local_path)
{
#if defined(PROSPERO_DATA_BRIDGE_ENABLED)
    std::lock_guard<std::mutex> lock(g_bridge_mutex);
    if (!g_bridge_ready.load() || file == RADIO_PAYLOAD_REPORT)
        return false;
    const bool result = PutLocalFile(file, local_path);
    if (!result)
        Report("persist snapshot id=%u FAIL errno=%d", static_cast<unsigned>(file), errno);
    return result;
#else
    (void)file;
    (void)local_path;
    return false;
#endif
}

bool radio_payload_bridge_pull_report()
{
#if defined(PROSPERO_DATA_BRIDGE_ENABLED)
    std::lock_guard<std::mutex> lock(g_bridge_mutex);
    return FetchPayloadReport();
#else
    return false;
#endif
}

bool radio_payload_bridge_keepalive()
{
#if defined(PROSPERO_DATA_BRIDGE_ENABLED)
    std::lock_guard<std::mutex> lock(g_bridge_mutex);
    if (!g_bridge_ready.load())
        return false;
    if (PingBridge())
        return true;
    Report("bridge keepalive FAIL; persistent storage disabled for this run errno=%d", errno);
    g_bridge_ready.store(false);
    return false;
#else
    return false;
#endif
}

bool radio_payload_bridge_aux_start()
{
#if defined(PROSPERO_DATA_BRIDGE_ENABLED)
    std::lock_guard<std::mutex> lock(g_bridge_mutex);
    if (!g_bridge_ready.load())
        return false;
    bool running = false;
    if (QueryAuxState(&running) && running)
        return true;
    if (!SendSimpleCommand(kStartAux))
        return false;
    return QueryAuxState(&running) && running;
#else
    return false;
#endif
}

bool radio_payload_bridge_aux_stop()
{
#if defined(PROSPERO_DATA_BRIDGE_ENABLED)
    std::lock_guard<std::mutex> lock(g_bridge_mutex);
    if (!g_bridge_ready.load())
        return false;
    bool running = false;
    if (QueryAuxState(&running) && !running)
        return true;
    if (!SendSimpleCommand(kStopAux))
        return false;
    return QueryAuxState(&running) && !running;
#else
    return false;
#endif
}

bool radio_payload_bridge_aux_running()
{
#if defined(PROSPERO_DATA_BRIDGE_ENABLED)
    std::lock_guard<std::mutex> lock(g_bridge_mutex);
    bool running = false;
    return QueryAuxState(&running) && running;
#else
    return false;
#endif
}

bool radio_payload_bridge_fetch_aux()
{
#if defined(PROSPERO_DATA_BRIDGE_ENABLED)
    std::lock_guard<std::mutex> lock(g_bridge_mutex);
    if (!g_bridge_ready.load())
        return false;
    bool not_found = false;
    const bool fetched = GetRemoteFile(RADIO_PAYLOAD_AUXLIST,
                                       "/download0/radio-aux.m3u", &not_found);
    Report("AUX list transfer from /data/radio %s",
           fetched ? "PASS" : not_found ? "NOT_FOUND" : "FAIL");
    return fetched;
#else
    return false;
#endif
}

void radio_payload_bridge_shutdown()
{
#if defined(PROSPERO_DATA_BRIDGE_ENABLED)
    std::lock_guard<std::mutex> lock(g_bridge_mutex);
    if (!g_bridge_ready.load())
    {
        CloseBridgeChannel();
        return;
    }
    const int socket_fd = BridgeChannel();
    if (socket_fd >= 0)
    {
        const bool aux_stopped = SendSimpleCommand(kStopAux);
        Report("AUX shutdown before bridge stop %s", aux_stopped ? "PASS" : "UNCONFIRMED");
        BridgeStatus status{};
        std::uint32_t response_size = 0;
        const bool sent = SendRequestHeader(socket_fd, kStop, 0, 0);
        const bool acknowledged = sent && ReceiveResponseHeader(socket_fd, &status, &response_size) &&
                                  status == kBridgeOk && response_size == 0;
        /* A half-close also requests teardown if STOP could not be acknowledged:
         * the helper's next stdin read sees EOF and its main returns. Keep the
         * read half open long enough to observe process exit and release. */
        (void)shutdown(socket_fd, SHUT_WR);
        const bool process_exited = WaitForPayloadExit(socket_fd);
        Report("bridge shutdown stop_ack=%s payload_exit_eof=%s",
               acknowledged ? "PASS" : "FAIL", process_exited ? "PASS" : "UNCONFIRMED");
    }
    CloseBridgeChannel();
#endif
}
