/* Prospero Radio - privileged data and AUX bridge launched by elfldr.
 * Copyright (C) 2026 BlackBearReloaded
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#include <arpa/inet.h>
#include <errno.h>
#include <fcntl.h>
#include <netinet/in.h>
#include <ps5/klog.h>
#include <signal.h>
#include <stdarg.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <sys/select.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/time.h>
#include <sys/types.h>
#include <time.h>
#include <unistd.h>

#define BRIDGE_NAME "ProsperoRadioDataBridge.elf"
#define DATA_DIRECTORY "/data/radio"
#define REPORT_PATH DATA_DIRECTORY "/prospero-payload-probe.log"
#define AUX_LIST_PATH DATA_DIRECTORY "/radio-aux.m3u"
#define BRIDGE_MAX_FILE (64U * 1024U * 1024U)
#define BRIDGE_CHUNK 32768U
#define AUX_MAX_BODY (32U * 1024U * 1024U)
#define RPC_PORT 7001
#define AUX_PORT 7000
#define BRIDGE_IDLE_SECONDS 120

enum bridge_operation
{
    BRIDGE_PING = 1,
    BRIDGE_GET = 2,
    BRIDGE_PUT = 3,
    BRIDGE_STOP = 4,
    BRIDGE_STATUS = 5,
    BRIDGE_START_AUX = 6,
    BRIDGE_STOP_AUX = 7,
    BRIDGE_ATTACH = 8
};

enum bridge_status
{
    BRIDGE_OK = 0,
    BRIDGE_NOT_FOUND = 1,
    BRIDGE_BAD_REQUEST = 2,
    BRIDGE_IO_ERROR = 3
};

static int g_data_directory_ok;
static int g_data_roundtrip_ok;
static int g_download_roundtrip_ok;
static int g_aux_ready;
static int g_parent_pid = -1;

static void report(const char *format, ...)
{
    char message[320];
    va_list args;
    va_start(args, format);
    int length = vsnprintf(message, sizeof(message), format, args);
    va_end(args);
    if (length < 0)
        return;
    if ((size_t)length >= sizeof(message))
        length = (int)sizeof(message) - 1;

    klog_printf("[" BRIDGE_NAME "] %s\n", message);
    int fd = open(REPORT_PATH, O_WRONLY | O_CREAT | O_APPEND, 0644);
    if (fd >= 0)
    {
        static const char prefix[] = "[PAYLOAD] ";
        (void)write(fd, prefix, sizeof(prefix) - 1);
        (void)write(fd, message, (size_t)length);
        (void)write(fd, "\n", 1);
        close(fd);
    }
    else
        klog_printf("[" BRIDGE_NAME "] report file open FAIL errno=%d\n", errno);
}

static int write_all(int fd, const void *data, size_t size)
{
    const char *bytes = (const char *)data;
    while (size > 0)
    {
        ssize_t written = write(fd, bytes, size);
        if (written < 0 && errno == EINTR)
            continue;
        if (written <= 0)
            return -1;
        bytes += written;
        size -= (size_t)written;
    }
    return 0;
}

static int send_all(int fd, const void *data, size_t size)
{
    const char *bytes = (const char *)data;
    while (size > 0)
    {
        ssize_t sent = send(fd, bytes, size, 0);
        if (sent < 0 && errno == EINTR)
            continue;
        if (sent <= 0)
            return -1;
        bytes += sent;
        size -= (size_t)sent;
    }
    return 0;
}

static int read_all(int fd, void *data, size_t size)
{
    char *bytes = (char *)data;
    while (size > 0)
    {
        ssize_t received = read(fd, bytes, size);
        if (received < 0 && errno == EINTR)
            continue;
        if (received <= 0)
        {
            if (received == 0)
                errno = ECONNRESET;
            return -1;
        }
        bytes += received;
        size -= (size_t)received;
    }
    return 0;
}

static int recv_all(int fd, void *data, size_t size)
{
    char *bytes = (char *)data;
    while (size > 0)
    {
        ssize_t received = recv(fd, bytes, size, 0);
        if (received < 0 && errno == EINTR)
            continue;
        if (received <= 0)
            return -1;
        bytes += received;
        size -= (size_t)received;
    }
    return 0;
}

static int open_listener(unsigned short port, int loopback, const char *name);

static uint32_t read_u32be(const unsigned char *data)
{
    return ((uint32_t)data[0] << 24) | ((uint32_t)data[1] << 16) |
           ((uint32_t)data[2] << 8) | (uint32_t)data[3];
}

static void write_u32be(unsigned char *data, uint32_t value)
{
    data[0] = (unsigned char)(value >> 24);
    data[1] = (unsigned char)(value >> 16);
    data[2] = (unsigned char)(value >> 8);
    data[3] = (unsigned char)value;
}

static int file_path(unsigned char file_id, char *path, size_t capacity)
{
    const char *name = NULL;
    switch (file_id)
    {
    case 1:
        name = "radio-browser.sqlite3";
        break;
    case 2:
        name = "radio-browser-favorites.bin";
        break;
    case 3:
        name = "radio-eq.txt";
        break;
    case 4:
        name = "radio-presets.bin";
        break;
    case 5:
        name = "prospero-payload-probe.log";
        break;
    case 6:
        name = "radio-aux.m3u";
        break;
    case 7:
        name = "radio-aux-favorites.bin";
        break;
    default:
        return -1;
    }
    int length = snprintf(path, capacity, "%s/%s", DATA_DIRECTORY, name);
    return length > 0 && (size_t)length < capacity ? 0 : -1;
}

static int send_response(int fd, unsigned char status, uint32_t size)
{
    unsigned char response[12] = {'P', 'R', 'P', 'C', 1, status, 0, 0, 0, 0, 0, 0};
    write_u32be(response + 8, size);
    return send_all(fd, response, sizeof(response));
}

static int serve_get(int connection, unsigned char file_id)
{
    char path[192];
    struct stat info;
    if (file_path(file_id, path, sizeof(path)) != 0)
        return send_response(connection, BRIDGE_BAD_REQUEST, 0);
    int fd = open(path, O_RDONLY);
    if (fd < 0)
        return send_response(connection, errno == ENOENT ? BRIDGE_NOT_FOUND : BRIDGE_IO_ERROR, 0);
    if (fstat(fd, &info) != 0 || !S_ISREG(info.st_mode) || info.st_size < 0 ||
        (uint64_t)info.st_size > BRIDGE_MAX_FILE)
    {
        close(fd);
        return send_response(connection, BRIDGE_IO_ERROR, 0);
    }
    uint32_t remaining = (uint32_t)info.st_size;
    if (send_response(connection, BRIDGE_OK, remaining) != 0)
    {
        close(fd);
        return -1;
    }
    char buffer[BRIDGE_CHUNK];
    while (remaining > 0)
    {
        size_t wanted = remaining < sizeof(buffer) ? remaining : sizeof(buffer);
        ssize_t count = read(fd, buffer, wanted);
        if (count < 0 && errno == EINTR)
            continue;
        if (count <= 0 || send_all(connection, buffer, (size_t)count) != 0)
        {
            close(fd);
            return -1;
        }
        remaining -= (uint32_t)count;
    }
    close(fd);
    return 0;
}

static int serve_put(int connection, unsigned char file_id, uint32_t size)
{
    char path[192];
    char temporary[208];
    if (file_path(file_id, path, sizeof(path)) != 0 || size > BRIDGE_MAX_FILE)
        return send_response(connection, BRIDGE_BAD_REQUEST, 0);
    int path_length = snprintf(temporary, sizeof(temporary), "%s.tmp", path);
    if (path_length <= 0 || (size_t)path_length >= sizeof(temporary))
        return send_response(connection, BRIDGE_BAD_REQUEST, 0);

    int fd = open(temporary, O_WRONLY | O_CREAT | O_TRUNC, 0644);
    if (fd < 0)
        return send_response(connection, BRIDGE_IO_ERROR, 0);
    char buffer[BRIDGE_CHUNK];
    uint32_t remaining = size;
    while (remaining > 0)
    {
        size_t amount = remaining < sizeof(buffer) ? remaining : sizeof(buffer);
        if (recv_all(connection, buffer, amount) != 0 || write_all(fd, buffer, amount) != 0)
        {
            close(fd);
            unlink(temporary);
            return -1;
        }
        remaining -= (uint32_t)amount;
    }
    if (fsync(fd) != 0)
    {
        close(fd);
        unlink(temporary);
        return send_response(connection, BRIDGE_IO_ERROR, 0);
    }
    if (close(fd) != 0 || rename(temporary, path) != 0)
    {
        unlink(temporary);
        return send_response(connection, BRIDGE_IO_ERROR, 0);
    }
    return send_response(connection, BRIDGE_OK, 0);
}

static int serve_bridge_request(int connection, int *stop_requested, int *aux_listener)
{
    unsigned char request[12];
    if (read_all(connection, request, sizeof(request)) != 0)
        return -1;
    if (memcmp(request, "PRPC", 4) != 0 || request[4] != 1 || request[7] != 0)
        return send_response(connection, BRIDGE_BAD_REQUEST, 0);

    unsigned char operation = request[5];
    unsigned char file_id = request[6];
    uint32_t size = read_u32be(request + 8);
    if (operation == BRIDGE_PING && file_id == 0 && size == 0)
        return send_response(connection, BRIDGE_OK, 0);
    if (operation == BRIDGE_GET && size == 0)
        return serve_get(connection, file_id);
    if (operation == BRIDGE_PUT)
        return serve_put(connection, file_id, size);
    if (operation == BRIDGE_STATUS && file_id == 0 && size == 0)
    {
        const unsigned char flags = g_aux_ready ? 1U : 0U;
        if (send_response(connection, BRIDGE_OK, 1) != 0)
            return -1;
        return send_all(connection, &flags, sizeof(flags));
    }
    if (operation == BRIDGE_START_AUX && file_id == 0 && size == 0)
    {
        if (*aux_listener < 0)
        {
            *aux_listener = open_listener(AUX_PORT, 0, "AUX");
            g_aux_ready = *aux_listener >= 0;
        }
        if (!g_aux_ready)
        {
            report("AUX start request failed; listener unavailable");
            return send_response(connection, BRIDGE_IO_ERROR, 0);
        }
        report("AUX server started by app API on port %d", AUX_PORT);
        return send_response(connection, BRIDGE_OK, 0);
    }
    if (operation == BRIDGE_STOP_AUX && file_id == 0 && size == 0)
    {
        if (*aux_listener >= 0)
        {
            close(*aux_listener);
            *aux_listener = -1;
        }
        g_aux_ready = 0;
        report("AUX server stopped by app API; listener released");
        return send_response(connection, BRIDGE_OK, 0);
    }
    if (operation == BRIDGE_ATTACH && file_id == 0 && size > 1U && size <= 0x7fffffffU)
    {
        g_parent_pid = (int)size;
        report("app ownership attached pid=%d", g_parent_pid);
        return send_response(connection, BRIDGE_OK, 0);
    }
    if (operation == BRIDGE_STOP && file_id == 0 && size == 0)
    {
        *stop_requested = 1;
        return send_response(connection, BRIDGE_OK, 0);
    }
    return send_response(connection, BRIDGE_BAD_REQUEST, 0);
}

static int probe_roundtrip(const char *directory, const char *name)
{
    char path[192];
    char contents[96];
    char readback[96];
    int path_length = snprintf(path, sizeof(path), "%s/.prospero-probe-%ld-%ld.tmp", directory,
                               (long)getpid(), (long)time(NULL));
    int content_length = snprintf(contents, sizeof(contents), "prospero-payload-bridge pid=%ld\n",
                                  (long)getpid());
    if (path_length <= 0 || (size_t)path_length >= sizeof(path) || content_length <= 0 ||
        (size_t)content_length >= sizeof(contents))
    {
        report("%s path formatting FAIL", name);
        return 0;
    }

    int fd = open(path, O_CREAT | O_EXCL | O_RDWR, 0600);
    if (fd < 0)
    {
        report("%s create FAIL errno=%d", name, errno);
        return 0;
    }
    int success = write_all(fd, contents, (size_t)content_length) == 0;
    if (success && lseek(fd, 0, SEEK_SET) < 0)
        success = 0;
    ssize_t read_count = success ? read(fd, readback, sizeof(readback)) : -1;
    if (read_count != content_length || memcmp(readback, contents, (size_t)content_length) != 0)
        success = 0;
    close(fd);
    if (unlink(path) != 0)
        success = 0;
    report("%s create-write-read-delete %s", name, success ? "PASS" : "FAIL");
    return success;
}

static void probe_storage(void)
{
    struct stat info;
    if (mkdir(DATA_DIRECTORY, 0755) == 0)
        report("mkdir %s PASS", DATA_DIRECTORY);
    else if (errno == EEXIST && stat(DATA_DIRECTORY, &info) == 0 && S_ISDIR(info.st_mode))
        report("directory %s already exists", DATA_DIRECTORY);
    else
        report("mkdir/stat %s FAIL errno=%d", DATA_DIRECTORY, errno);

    g_data_directory_ok = stat(DATA_DIRECTORY, &info) == 0 && S_ISDIR(info.st_mode);
    if (g_data_directory_ok)
    {
        report("directory %s accessible PASS", DATA_DIRECTORY);
        g_data_roundtrip_ok = probe_roundtrip(DATA_DIRECTORY, DATA_DIRECTORY);
    }
    g_download_roundtrip_ok = probe_roundtrip("/download0", "/download0");
}

static int open_listener(unsigned short port, int loopback, const char *name)
{
    int listener = socket(AF_INET, SOCK_STREAM, 0);
    if (listener < 0)
    {
        report("%s socket FAIL errno=%d", name, errno);
        return -1;
    }
    int reuse = 1;
    if (setsockopt(listener, SOL_SOCKET, SO_REUSEADDR, &reuse, sizeof(reuse)) != 0)
    {
        report("%s SO_REUSEADDR FAIL errno=%d", name, errno);
        close(listener);
        return -1;
    }

    struct sockaddr_in address;
    memset(&address, 0, sizeof(address));
    address.sin_len = sizeof(address);
    address.sin_family = AF_INET;
    address.sin_port = htons(port);
    address.sin_addr.s_addr = htonl(loopback ? INADDR_LOOPBACK : INADDR_ANY);
    if (bind(listener, (const struct sockaddr *)&address, sizeof(address)) != 0)
    {
        report("%s bind port %u FAIL errno=%d", name, (unsigned)port, errno);
        close(listener);
        return -1;
    }
    if (listen(listener, 4) != 0)
    {
        report("%s listen port %u FAIL errno=%d", name, (unsigned)port, errno);
        close(listener);
        return -1;
    }
    report("%s listening port %u", name, (unsigned)port);
    return listener;
}

static int aux_send_response(int connection, const char *status, const char *content_type,
                             const void *body, size_t body_size)
{
    char header[256];
    int header_size = snprintf(header, sizeof(header),
                               "HTTP/1.1 %s\r\nContent-Type: %s\r\n"
                               "Content-Length: %lu\r\nConnection: close\r\n\r\n",
                               status, content_type, (unsigned long)body_size);
    if (header_size <= 0 || (size_t)header_size >= sizeof(header) ||
        send_all(connection, header, (size_t)header_size) != 0)
        return -1;
    return body_size == 0 ? 0 : send_all(connection, body, body_size);
}

static int aux_send_text(int connection, const char *status, const char *body)
{
    return aux_send_response(connection, status, "text/plain; charset=utf-8", body, strlen(body));
}

static int header_name_is(const char *begin, size_t length, const char *name)
{
    while (length > 0 && (begin[length - 1] == ' ' || begin[length - 1] == '\t'))
        --length;
    return strlen(name) == length && strncasecmp(begin, name, length) == 0;
}

static int parse_content_length(const char *headers, size_t header_size, size_t *out_length)
{
    const char *line = strstr(headers, "\r\n");
    if (line == NULL)
        return -1;
    line += 2;
    const char *limit = headers + header_size;
    while (line < limit && line[0] != '\r' && line[1] != '\n')
    {
        const char *end = strstr(line, "\r\n");
        if (end == NULL || end > limit)
            return -1;
        const char *colon = memchr(line, ':', (size_t)(end - line));
        if (colon != NULL && header_name_is(line, (size_t)(colon - line), "Content-Length"))
        {
            const char *value = colon + 1;
            while (value < end && (*value == ' ' || *value == '\t'))
                ++value;
            if (value == end || *value == '-')
                return -1;
            char *number_end = NULL;
            unsigned long long parsed = strtoull(value, &number_end, 10);
            while (number_end < end && (*number_end == ' ' || *number_end == '\t'))
                ++number_end;
            if (number_end != end || parsed > AUX_MAX_BODY)
                return -1;
            *out_length = (size_t)parsed;
            return 0;
        }
        line = end + 2;
    }
    return 1;
}

static int aux_store_list(int connection, char *request, size_t received, size_t header_size)
{
    if (strncmp(request, "POST /list ", 11) != 0)
        return aux_send_text(connection, "404 Not Found", "Ruta no encontrada");

    size_t body_size = 0;
    const int length_result = parse_content_length(request, header_size, &body_size);
    if (length_result == 1)
        return aux_send_text(connection, "411 Length Required", "Falta Content-Length");
    if (length_result != 0 || body_size == 0)
        return aux_send_text(connection, "400 Bad Request", "Longitud de lista invalida o vacia");

    size_t initial_body = received - header_size;
    if (initial_body > body_size)
        return aux_send_text(connection, "400 Bad Request", "Longitud de lista invalida");

    char temporary[sizeof(AUX_LIST_PATH) + 5];
    int temporary_size = snprintf(temporary, sizeof(temporary), "%s.tmp", AUX_LIST_PATH);
    if (temporary_size <= 0 || (size_t)temporary_size >= sizeof(temporary))
        return aux_send_text(connection, "500 Internal Server Error", "Error de ruta");
    int output = open(temporary, O_WRONLY | O_CREAT | O_TRUNC, 0644);
    if (output < 0)
        return aux_send_text(connection, "500 Internal Server Error", "No se pudo crear la lista");

    int ok = initial_body == 0 || write_all(output, request + header_size, initial_body) == 0;
    size_t remaining = body_size - initial_body;
    char buffer[BRIDGE_CHUNK];
    while (ok && remaining > 0)
    {
        size_t wanted = remaining < sizeof(buffer) ? remaining : sizeof(buffer);
        ssize_t count = recv(connection, buffer, wanted, 0);
        if (count < 0 && errno == EINTR)
            continue;
        if (count <= 0 || write_all(output, buffer, (size_t)count) != 0)
        {
            ok = 0;
            break;
        }
        remaining -= (size_t)count;
    }
    if (ok && fsync(output) != 0)
        ok = 0;
    if (close(output) != 0)
        ok = 0;
    if (ok && rename(temporary, AUX_LIST_PATH) != 0)
        ok = 0;
    if (!ok)
    {
        unlink(temporary);
        return aux_send_text(connection, "400 Bad Request",
                             "Carga incompleta; se conserva la lista anterior");
    }
    report("AUX M3U stored bytes=%lu", (unsigned long)body_size);
    return aux_send_text(connection, "201 Created", "Lista recibida; pulsa BARRIDO");
}

static void handle_aux_client(int connection)
{
    struct timeval timeout = {30, 0};
    (void)setsockopt(connection, SOL_SOCKET, SO_RCVTIMEO, &timeout, sizeof(timeout));
    (void)setsockopt(connection, SOL_SOCKET, SO_SNDTIMEO, &timeout, sizeof(timeout));
    static const char page[] =
        "<!doctype html><html lang='es'><head><meta charset='utf-8'>"
        "<meta name='viewport' content='width=device-width,initial-scale=1'>"
        "<title>Prospero Radio | AUX</title><style>"
        ":root{color-scheme:dark;--amber:#f2b965;--gold:#c9843d;--ink:#17110d;--steel:#69727a;--light:#d2d0c8}"
        "*{box-sizing:border-box}body{margin:0;min-height:100vh;padding:28px 14px;color:#e9e5dd;font:15px/1.5 system-ui,-apple-system,'Segoe UI',sans-serif;"
        "background:radial-gradient(ellipse at 50% 0,#34261b 0,#17110d 58%,#0b0908 100%)}"
        ".receiver{max-width:1120px;margin:18px auto;padding:10px;border:1px solid #98693f;border-radius:16px;"
        "background:repeating-linear-gradient(90deg,#321f16 0,#704a32 3px,#392219 7px,#563624 11px,#2b1a13 16px);box-shadow:0 24px 60px #000c,inset 0 0 0 2px #130d09}"
        ".back{position:relative;overflow:hidden;padding:22px clamp(18px,4vw,42px) 28px;border:1px solid #a7adb0;border-radius:9px;"
        "background:repeating-linear-gradient(180deg,#737b80 0,#6b747a 2px,#626b71 4px,#70787e 7px);box-shadow:inset 0 1px 0 #e2e0d9,inset 0 -8px 20px #202326}"
        ".top{display:flex;justify-content:space-between;align-items:center;gap:16px;padding:0 0 14px;border-bottom:1px solid #3c4246}"
        ".brand{margin:0;color:#f0eee8;font-size:clamp(22px,4vw,32px);letter-spacing:.1em;text-shadow:0 1px 2px #000}.model{color:#30383c;font-size:12px;letter-spacing:.14em;text-transform:uppercase}"
        ".vent{height:17px;margin:14px 0 20px;border:1px solid #33393d;border-radius:3px;background:repeating-linear-gradient(90deg,#1f2427 0,#1f2427 3px,#92999b 4px,#92999b 5px);box-shadow:inset 0 2px 5px #000}"
        ".layout{display:grid;grid-template-columns:minmax(250px,.8fr) minmax(360px,1.4fr);gap:18px;align-items:stretch}"
        ".ports,.import{border:1px solid #3b4247;border-radius:6px;background:linear-gradient(145deg,#596269,#424a50);box-shadow:inset 0 1px 0 #aeb4b4,0 3px 8px #0005}"
        ".ports{padding:18px}.label{color:#d3d0c8;font-size:12px;font-weight:700;letter-spacing:.12em;text-transform:uppercase}.rcas{display:flex;gap:20px;margin:19px 0 15px}"
        ".rca{display:flex;align-items:center;gap:10px}.socket{position:relative;width:48px;height:48px;border:4px solid #c5c1b5;border-radius:50%;background:radial-gradient(circle,#0b0c0d 0 34%,#252a2d 36% 52%,#0c0e0f 56%);box-shadow:0 2px 5px #000,inset 0 0 0 2px #53595c}"
        ".socket:after{content:'';position:absolute;left:17px;top:17px;width:7px;height:7px;border-radius:50%;background:#090909}.red{border-color:#b5533d}.white{border-color:#d5d0c4}"
        ".rca b{display:block;color:#eee6d8;font-size:13px;letter-spacing:.08em}.rca small{display:block;color:#bbc0bf;font-size:11px}"
        ".jack{display:flex;align-items:center;gap:9px;margin-top:12px;padding-top:13px;border-top:1px solid #30373b;color:#d8d5cc;font-size:12px}.jack-dot{width:17px;height:17px;border:3px solid #bbb8ae;border-radius:50%;background:#101112;box-shadow:inset 0 0 0 2px #353b3f}"
        ".import{padding:18px 20px;background:linear-gradient(145deg,#29201a,#17120f);border-color:#8d6846;color:var(--amber);font-family:ui-monospace,'Cascadia Mono',Consolas,monospace}"
        ".import h2{margin:4px 0 2px;font-size:22px}.import p{margin:0 0 14px;color:#c4a078;font-size:13px}.aux-id{color:#df9a4e;font-size:11px;letter-spacing:.16em}"
        ".upload{display:block;padding:16px 12px;border:1px dashed #bd874d;border-radius:5px;text-align:center;cursor:pointer;background:#211811}.upload:hover{border-color:#ffd28a;background:#2d2117}"
        ".upload input{position:absolute;width:1px;height:1px;opacity:0;overflow:hidden}.upload b{display:block;color:#ffd08a;font-size:16px}.upload span{display:block;margin-top:3px;color:#b89a77;font-size:12px}"
        ".file-state{min-height:20px;margin:7px 0 10px;color:#c1a47f;font-size:12px}.paste{margin:8px 0;color:#c1a47f;font-size:12px;cursor:pointer}textarea{display:block;width:100%;min-height:108px;margin-top:8px;padding:9px;resize:vertical;"
        "border:1px solid #67503a;border-radius:4px;color:#f1e7d9;background:#100d0b;font:12px/1.4 ui-monospace,Consolas,monospace}"
        ".actions{display:flex;align-items:center;gap:12px;flex-wrap:wrap;margin-top:12px}button{border:1px solid #f4be76;border-radius:4px;padding:10px 16px;color:#26190d;background:linear-gradient(#ffd18b,#d79543);font-weight:750;letter-spacing:.04em;cursor:pointer}button:disabled{opacity:.6;cursor:wait}"
        ".status{min-height:22px;color:#d1aa77;font-size:12px}.status.ok{color:#9bd3a9}.status.error{color:#f08c75}.note{margin:13px 0 0;padding-top:10px;border-top:1px solid #42372c;color:#9e8d78;font:11px/1.4 system-ui,sans-serif}"
        ".screws{position:absolute;top:9px;width:7px;height:7px;border:1px solid #373d40;border-radius:50%;background:#a7abad;box-shadow:inset 0 0 0 2px #60686b}.s1{left:9px}.s2{right:9px}"
        "@media(max-width:720px){body{padding:10px 7px}.receiver{padding:6px}.back{padding:16px 12px}.top{align-items:flex-start;flex-direction:column;gap:2px}.layout{grid-template-columns:1fr}.ports{padding:13px}.rcas{margin:12px 0}.socket{width:42px;height:42px}}"
        "</style></head><body><main class='receiver'><section class='back'><i class='screws s1'></i><i class='screws s2'></i>"
        "<header class='top'><h1 class='brand'>PROSPERO RADIO</h1><span class='model'>Panel trasero · receptor de Internet · AUX</span></header><div class='vent' aria-hidden='true'></div>"
        "<div class='layout'><section class='ports'><div class='label'>AUX INPUT · ANALOG IN</div><div class='rcas'>"
        "<div class='rca'><span class='socket white'></span><span><b>L · LEFT</b><small>AUX IN</small></span></div>"
        "<div class='rca'><span class='socket red'></span><span><b>R · RIGHT</b><small>AUX IN</small></span></div></div>"
        "<div class='jack'><span class='jack-dot'></span><span>3.5 mm · LINE INPUT</span></div><p class='note'>Entradas ilustradas del equipo. La importación de emisoras se realiza por la red local.</p></section>"
        "<section class='import'><div class='aux-id'>AUX / NETWORK BRIDGE · 7000</div><h2>Importar lista de emisoras</h2><p>Selecciona una lista M3U; la radio la recibirá y podrás explorarla en BARRIDO.</p>"
        "<label class='upload' for='file'><input id='file' type='file' accept='.m3u,.m3u8,audio/x-mpegurl,application/vnd.apple.mpegurl,text/plain'><b>SELECCIONAR ARCHIVO .M3U</b><span>Buscar en este dispositivo</span></label>"
        "<div id='file-state' class='file-state'>Ningún archivo seleccionado</div><details><summary class='paste'>O pegar el contenido M3U</summary>"
        "<textarea id='text' spellcheck='false' placeholder='#EXTM3U&#10;#EXTINF:-1,Nombre de la emisora&#10;https://servidor/stream'></textarea></details>"
        "<div class='actions'><button id='send' type='button'>ENVIAR A PROSPERO RADIO</button><span id='status' class='status' role='status' aria-live='polite'></span></div></section></div></section></main>"
        "<script>(()=>{const file=document.querySelector('#file'),text=document.querySelector('#text'),label=document.querySelector('#file-state'),status=document.querySelector('#status'),button=document.querySelector('#send');"
        "file.addEventListener('change',async()=>{const f=file.files&&file.files[0];if(!f){label.textContent='Ningún archivo seleccionado';return}"
        "try{const body=await f.text(),urls=body.split(/\\r?\\n/).filter(x=>/^\\s*https?:\\/\\//i.test(x)).length;label.textContent=f.name+' · '+(urls?urls+' URL(s) detectadas':'sin URLs HTTP(S)');status.textContent=''}catch(e){label.textContent='No se pudo leer el archivo'}});"
        "button.addEventListener('click',async()=>{const f=file.files&&file.files[0];let body='';try{body=f?await f.text():text.value}catch(e){status.className='status error';status.textContent='No se pudo leer el archivo';return}"
        "const urls=body.split(/\\r?\\n/).filter(x=>/^\\s*https?:\\/\\//i.test(x)).length;if(!urls){status.className='status error';status.textContent='No hay URLs HTTP o HTTPS en la lista';return}"
        "button.disabled=true;status.className='status';status.textContent='Enviando '+urls+' URL(s)…';try{const r=await fetch('/list',{method:'POST',headers:{'Content-Type':'audio/x-mpegurl'},body});const answer=await r.text();"
        "status.className=r.ok?'status ok':'status error';status.textContent=r.ok?answer:('Error '+r.status+': '+answer)}catch(e){status.className='status error';status.textContent='No se pudo conectar con la consola'}finally{button.disabled=false}})})();</script></body></html>";
    char request[8193];
    size_t received = 0;
    size_t header_size = 0;
    while (received < sizeof(request) - 1 && header_size == 0)
    {
        ssize_t count = recv(connection, request + received, sizeof(request) - 1 - received, 0);
        if (count < 0 && errno == EINTR)
            continue;
        if (count <= 0)
            break;
        received += (size_t)count;
        request[received] = '\0';
        char *separator = strstr(request, "\r\n\r\n");
        if (separator != NULL)
            header_size = (size_t)(separator - request) + 4;
    }
    if (header_size == 0)
        (void)aux_send_text(connection, "400 Bad Request", "Cabecera HTTP incompleta");
    else if (strncmp(request, "POST /list ", 11) == 0)
        (void)aux_store_list(connection, request, received, header_size);
    else if (strncmp(request, "GET /", 5) == 0 || strncmp(request, "HEAD /", 6) == 0)
        (void)aux_send_response(connection, "200 OK", "text/html; charset=utf-8", page,
                                sizeof(page) - 1);
    else
        (void)aux_send_text(connection, "404 Not Found", "Ruta no encontrada");
    close(connection);
}

static int parse_parent_pid(int argc, char **argv)
{
    if (argc < 2 || argv[1] == NULL || argv[1][0] == '\0')
        return -1;
    char *end = NULL;
    long value = strtol(argv[1], &end, 10);
    if (end == argv[1] || *end != '\0' || value <= 1 || value > 0x7fffffffL)
        return -1;
    return (int)value;
}

static int run_servers(void)
{
    int rpc_listener = open_listener(RPC_PORT, 1, "RPC");
    if (rpc_listener < 0)
        return -1;
    int aux_listener = -1;
    g_aux_ready = 0;
    report("AUX server is stopped until the app requests it over RPC");

    int bridge_client = -1;
    time_t last_activity = time(NULL);
    int stop_requested = 0;
    while (!stop_requested)
    {
        fd_set readable;
        FD_ZERO(&readable);
        FD_SET(rpc_listener, &readable);
        int maximum_fd = rpc_listener;
        if (aux_listener >= 0)
        {
            FD_SET(aux_listener, &readable);
            if (aux_listener > maximum_fd)
                maximum_fd = aux_listener;
        }
        if (bridge_client >= 0)
        {
            FD_SET(bridge_client, &readable);
            if (bridge_client > maximum_fd)
                maximum_fd = bridge_client;
        }
        struct timeval timeout = {1, 0};
        int selected = select(maximum_fd + 1, &readable, NULL, NULL, &timeout);
        if (selected < 0)
        {
            if (errno == EINTR)
                continue;
            report("select failed errno=%d", errno);
            break;
        }
        if (selected > 0 && FD_ISSET(rpc_listener, &readable))
        {
            int client = accept(rpc_listener, NULL, NULL);
            if (client >= 0)
            {
                if (bridge_client >= 0)
                {
                    report("RPC rejected duplicate local client");
                    close(client);
                }
                else
                {
                    struct timeval client_timeout = {30, 0};
                    (void)setsockopt(client, SOL_SOCKET, SO_RCVTIMEO,
                                     &client_timeout, sizeof(client_timeout));
                    (void)setsockopt(client, SOL_SOCKET, SO_SNDTIMEO,
                                     &client_timeout, sizeof(client_timeout));
                    bridge_client = client;
                    last_activity = time(NULL);
                    report("RPC app channel connected");
                }
            }
            else if (errno != EINTR)
                report("RPC accept failed errno=%d", errno);
        }
        if (selected > 0 && aux_listener >= 0 && FD_ISSET(aux_listener, &readable))
        {
            int client = accept(aux_listener, NULL, NULL);
            if (client >= 0)
            {
                handle_aux_client(client);
                last_activity = time(NULL);
            }
            else if (errno != EINTR)
                report("AUX accept failed errno=%d", errno);
        }
        if (selected > 0 && bridge_client >= 0 && FD_ISSET(bridge_client, &readable))
        {
            int result = serve_bridge_request(bridge_client, &stop_requested, &aux_listener);
            if (result != 0)
            {
                if (errno != 0)
                    report("RPC client disconnected or request failed errno=%d", errno);
                close(bridge_client);
                bridge_client = -1;
            }
            else
                last_activity = time(NULL);
        }

        if (g_parent_pid > 1 && kill(g_parent_pid, 0) != 0 && errno == ESRCH)
        {
            report("owning app pid=%d exited; stopping bridge", g_parent_pid);
            break;
        }
        if (bridge_client < 0 && time(NULL) - last_activity >= BRIDGE_IDLE_SECONDS)
        {
            report("bridge idle timeout; stopping helper and releasing sockets");
            break;
        }
    }

    if (bridge_client >= 0)
    {
        (void)shutdown(bridge_client, SHUT_RDWR);
        close(bridge_client);
    }
    if (aux_listener >= 0)
        close(aux_listener);
    close(rpc_listener);
    report("bridge shutdown complete; listeners released");
    return 0;
}

int main(int argc, char **argv)
{
    g_parent_pid = parse_parent_pid(argc, argv);
    (void)signal(SIGPIPE, SIG_IGN);
    struct stat data_directory;
    if ((mkdir(DATA_DIRECTORY, 0755) == 0 || errno == EEXIST) &&
        stat(DATA_DIRECTORY, &data_directory) == 0 && S_ISDIR(data_directory.st_mode))
    {
        int report_fd = open(REPORT_PATH, O_WRONLY | O_CREAT | O_TRUNC, 0644);
        if (report_fd >= 0)
            close(report_fd);
    }

    report("startup name=%s pid=%ld parent=%d", BRIDGE_NAME, (long)getpid(), g_parent_pid);
    probe_storage();
    if (!g_data_directory_ok || !g_data_roundtrip_ok)
    {
        report("persistent data unavailable; refusing to run network services");
        return 1;
    }
    int result = run_servers();
    report("process exit result=%d data_dir=%s data_rw=%s download0_rw=%s aux=%s",
           result, g_data_directory_ok ? "PASS" : "FAIL",
           g_data_roundtrip_ok ? "PASS" : "FAIL",
           g_download_roundtrip_ok ? "PASS" : "FAIL",
           g_aux_ready ? "READY" : "FAILED");
    return result == 0 ? 0 : 1;
}
