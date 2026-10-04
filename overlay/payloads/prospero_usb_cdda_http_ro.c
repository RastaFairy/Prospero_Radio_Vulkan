/* Read-only CD-DA to virtual WAV HTTP source for elfldr. */
/* Copyright (C) 2026 BlackBearReloaded */
/* SPDX-License-Identifier: GPL-3.0-or-later */

#include <sys/types.h>
#include <sys/socket.h>

#include <stdio.h>
#include <cam/cam.h>
#include <cam/cam_ccb.h>
#include <cam/scsi/scsi_all.h>
#include <cam/scsi/scsi_pass.h>
#include <netinet/in.h>
#include <ps5/klog.h>

#include <ctype.h>
#include <errno.h>
#include <fcntl.h>
#include <stdint.h>
#include <stdarg.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ioctl.h>
#include <sys/time.h>
#include <time.h>
#include <unistd.h>

#define CD_DEVICE_PATH "/dev/cd1"
#define HTTP_PORT 39090U
#define MAX_TRACKS 99U
#define CDDA_BYTES_PER_SECTOR 2352U
#define READ_BATCH_SECTORS 20U
#define READ_BATCH_BYTES (CDDA_BYTES_PER_SECTOR * READ_BATCH_SECTORS)
#define MAX_HTTP_REQUEST 1024U
#define CDROM_LEADOUT_TRACK 0xaaU
#define HTTP_IDLE_TIMEOUT_SECONDS 300U

struct track_info {
    uint8_t number;
    uint8_t control;
    uint32_t start_lba;
    uint32_t end_lba;
    int valid;
};

static int g_cd_fd = -1;
static int g_pass_fd = -1;
static path_id_t g_path_id;
static target_id_t g_target_id;
static lun_id_t g_target_lun;
static char g_pass_path[64];
static size_t g_last_data_transfer_length;

static void report(const char *format, ...)
{
    char message[700];
    va_list args;
    va_start(args, format);
    int length = vsnprintf(message, sizeof(message), format, args);
    va_end(args);
    if (length < 0)
        return;
    message[sizeof(message) - 1U] = '\0';
    klog_printf("[USB-CDDA-HTTP-RO] %s\n", message);
}

static uint32_t read_be32(const uint8_t *bytes)
{
    return ((uint32_t)bytes[0] << 24) | ((uint32_t)bytes[1] << 16) |
           ((uint32_t)bytes[2] << 8) | (uint32_t)bytes[3];
}

static void hex_bytes(const uint8_t *bytes, size_t size, char *output,
                      size_t capacity)
{
    size_t used = 0;
    if (capacity == 0)
        return;
    output[0] = '\0';
    for (size_t i = 0; i < size; ++i) {
        if (used + 4U >= capacity)
            break;
        int count = snprintf(output + used, capacity - used, "%s%02x",
                             i == 0 ? "" : " ", (unsigned int)bytes[i]);
        if (count <= 0)
            break;
        used += (size_t)count;
    }
}

static uint32_t fnv1a_update(uint32_t hash, const uint8_t *bytes, size_t size)
{
    for (size_t i = 0; i < size; ++i) {
        hash ^= bytes[i];
        hash *= 16777619U;
    }
    return hash;
}

static int open_devices(void)
{
    g_cd_fd = open(CD_DEVICE_PATH, O_RDONLY);
    if (g_cd_fd < 0) {
        report("OPEN_CD result=FAIL path=%s errno=%d (%s)", CD_DEVICE_PATH,
               errno, strerror(errno));
        return -1;
    }
    report("OPEN_CD result=PASS path=%s mode=readonly", CD_DEVICE_PATH);

    union ccb ccb;
    memset(&ccb, 0, sizeof(ccb));
    ccb.ccb_h.func_code = XPT_GDEVLIST;
    if (ioctl(g_cd_fd, CAMGETPASSTHRU, &ccb) != 0 ||
        ccb.cgdl.status == CAM_GDEVLIST_ERROR) {
        report("CAMGETPASSTHRU cd=%s result=FAIL errno=%d", CD_DEVICE_PATH,
               errno);
        return -1;
    }

    char peripheral[DEV_IDLEN + 1U];
    memcpy(peripheral, ccb.cgdl.periph_name, DEV_IDLEN);
    peripheral[DEV_IDLEN] = '\0';
    int length = snprintf(g_pass_path, sizeof(g_pass_path), "/dev/%s%u",
                          peripheral, ccb.cgdl.unit_number);
    if (length <= 0 || (size_t)length >= sizeof(g_pass_path)) {
        report("CAMGETPASSTHRU returned invalid pass path");
        return -1;
    }

    /* PS5 requires O_RDWR for pass(4), but the CDB allowlist below only
     * permits input/read commands. No SCSI write or console file write exists. */
    g_pass_fd = open(g_pass_path, O_RDWR);
    if (g_pass_fd < 0) {
        report("OPEN_PASS result=FAIL path=%s errno=%d (%s)", g_pass_path,
               errno, strerror(errno));
        return -1;
    }

    memset(&ccb, 0, sizeof(ccb));
    ccb.ccb_h.func_code = XPT_GDEVLIST;
    if (ioctl(g_pass_fd, CAMGETPASSTHRU, &ccb) != 0 ||
        ccb.cgdl.status == CAM_GDEVLIST_ERROR) {
        report("CAMGETPASSTHRU pass=%s result=FAIL errno=%d", g_pass_path,
               errno);
        return -1;
    }
    g_path_id = ccb.ccb_h.path_id;
    g_target_id = ccb.ccb_h.target_id;
    g_target_lun = ccb.ccb_h.target_lun;
    report("OPEN_PASS result=PASS path=%s tuple=%u:%u:%llu", g_pass_path,
           (unsigned int)g_path_id, (unsigned int)g_target_id,
           (unsigned long long)g_target_lun);
    return 0;
}

static int issue_read(const char *name, const uint8_t *cdb, uint8_t cdb_length,
                      uint8_t *data, size_t data_length, int log_success)
{
    if (cdb == NULL || cdb_length == 0 || cdb_length > CAM_MAX_CDBLEN ||
        data_length > UINT32_MAX || g_pass_fd < 0)
        return -1;
    switch (cdb[0]) {
    case 0x00U: /* TEST UNIT READY */
        if (data_length == 0)
            break;
        return -1;
    case 0x12U: /* INQUIRY */
    case 0x43U: /* READ TOC/PMA/ATIP */
    case 0x46U: /* GET CONFIGURATION */
    case 0xbeU: /* READ CD */
        break;
    default:
        report("SCSI_BLOCKED opcode=%#x (not on read-only allowlist)",
               (unsigned int)cdb[0]);
        return -1;
    }

    char cdb_text[96];
    hex_bytes(cdb, cdb_length, cdb_text, sizeof(cdb_text));
    if (log_success)
        report("SCSI_BEGIN name=%s cdb=%s dxfer=%u", name, cdb_text,
               (unsigned int)data_length);

    union ccb ccb;
    memset(&ccb, 0, sizeof(ccb));
    ccb.ccb_h.path_id = g_path_id;
    ccb.ccb_h.target_id = g_target_id;
    ccb.ccb_h.target_lun = g_target_lun;
    ccb.ccb_h.flags = data_length == 0 ? CAM_DIR_NONE : CAM_DIR_IN;
    ccb.ccb_h.flags |= CAM_DEV_QFRZDIS;
    memcpy(ccb.csio.cdb_io.cdb_bytes, cdb, cdb_length);
    cam_fill_csio(&ccb.csio, 0, NULL, ccb.ccb_h.flags, CAM_TAG_ACTION_NONE,
                  data, (uint32_t)data_length,
                  (uint8_t)sizeof(ccb.csio.sense_data), cdb_length, 30000U);
    ccb.ccb_h.path_id = g_path_id;
    ccb.ccb_h.target_id = g_target_id;
    ccb.ccb_h.target_lun = g_target_lun;

    int result = ioctl(g_pass_fd, CAMIOCOMMAND, &ccb);
    int saved_errno = result == 0 ? 0 : errno;
    uint32_t cam_status = ccb.ccb_h.status & CAM_STATUS_MASK;
    g_last_data_transfer_length = ccb.csio.resid <= data_length
                                      ? data_length - ccb.csio.resid
                                      : 0;
    int transfer_valid = ccb.csio.resid == 0 ||
                         (cdb[0] == 0x43U && g_last_data_transfer_length >= 4U);
    int complete = result == 0 && cam_status == CAM_REQ_CMP &&
                   ccb.csio.scsi_status == 0 && transfer_valid;
    if (log_success || !complete) {
        uint8_t sense[24];
        size_t sense_size = ccb.csio.sense_len;
        if (sense_size > sizeof(sense))
            sense_size = sizeof(sense);
        memcpy(sense, &ccb.csio.sense_data, sense_size);
        char sense_text[3U * sizeof(sense) + 1U];
        hex_bytes(sense, sense_size, sense_text, sizeof(sense_text));
        report("SCSI_END name=%s result=%s errno=%d cam=%#x scsi=%#x resid=%u sense=%s",
               name, complete ? "PASS" : "FAIL", saved_errno, cam_status,
               (unsigned int)ccb.csio.scsi_status,
               (unsigned int)ccb.csio.resid,
               sense_size == 0 ? "none" : sense_text);
        if (sense_size >= 14U && (sense[0] & 0x7eU) == 0x70U)
            report("SCSI_SENSE name=%s key=%#x asc=%#x ascq=%#x", name,
                   (unsigned int)(sense[2] & 0x0fU),
                   (unsigned int)sense[12], (unsigned int)sense[13]);
        else if (sense_size >= 4U && (sense[0] & 0x7eU) == 0x72U)
            report("SCSI_SENSE name=%s key=%#x asc=%#x ascq=%#x", name,
                   (unsigned int)(sense[1] & 0x0fU),
                   (unsigned int)sense[2], (unsigned int)sense[3]);
    }
    return complete ? 0 : -1;
}

static int read_toc(struct track_info *tracks, size_t *track_count,
                    uint32_t *leadout_lba)
{
    uint8_t cdb[10] = {0x43U, 0, 0, 0, 0, 0, 0, 0, 0, 0};
    uint8_t data[804];
    memset(data, 0, sizeof(data));
    cdb[7] = (uint8_t)(sizeof(data) >> 8);
    cdb[8] = (uint8_t)sizeof(data);
    if (issue_read("READ_TOC_FORMAT_0", cdb, sizeof(cdb), data, sizeof(data), 1) != 0)
        return -1;

    uint16_t response_length = (uint16_t)(((uint16_t)data[0] << 8) | data[1]);
    uint8_t first = data[2];
    uint8_t last = data[3];
    size_t transfer_length = g_last_data_transfer_length;
    /* Some pass(4) implementations report the allocation remainder as resid;
     * the TOC response length is authoritative when the command itself passed. */
    if (response_length < 4U || first == 0 || last < first ||
        (unsigned int)(last - first) >= MAX_TRACKS) {
        report("TOC_INVALID length=%u first=%u last=%u",
               (unsigned int)response_length, (unsigned int)first,
               (unsigned int)last);
        return -1;
    }
    size_t declared_length = (size_t)response_length + 2U;
    if (declared_length < transfer_length)
        transfer_length = declared_length;
    if (transfer_length < 4U) {
        report("TOC_INVALID transferred=%u", (unsigned int)transfer_length);
        return -1;
    }

    memset(tracks, 0, sizeof(*tracks) * MAX_TRACKS);
    size_t count = (size_t)(last - first) + 1U;
    size_t descriptors = ((size_t)response_length + 2U) / 8U;
    if (descriptors > (transfer_length - 4U) / 8U)
        descriptors = (transfer_length - 4U) / 8U;
    *leadout_lba = 0;
    for (size_t i = 0; i < descriptors; ++i) {
        const uint8_t *entry = data + 4U + i * 8U;
        uint8_t number = entry[2];
        if (number == CDROM_LEADOUT_TRACK) {
            *leadout_lba = read_be32(entry + 4U);
        } else if (number >= first && number <= last) {
            size_t index = (size_t)(number - first);
            tracks[index].number = number;
            tracks[index].control = (uint8_t)(entry[1] & 0x0fU);
            tracks[index].start_lba = read_be32(entry + 4U);
            tracks[index].valid = 1;
        }
    }
    if (*leadout_lba == 0) {
        report("TOC_INVALID no leadout descriptor descriptors=%u",
               (unsigned int)descriptors);
        return -1;
    }

    *track_count = count;
    for (size_t i = 0; i < count; ++i) {
        if (!tracks[i].valid)
            continue;
        uint32_t end = *leadout_lba;
        for (size_t next = i + 1U; next < count; ++next) {
            if (tracks[next].valid) {
                end = tracks[next].start_lba;
                break;
            }
        }
        tracks[i].end_lba = end;
        report("TOC_TRACK track=%u control=%#x type=%s start=%u end=%u sectors=%u",
               (unsigned int)tracks[i].number, (unsigned int)tracks[i].control,
               (tracks[i].control & 0x04U) != 0 ? "data" : "audio",
               tracks[i].start_lba, end, end - tracks[i].start_lba);
    }
    report("TOC_SUMMARY tracks=%u leadout=%u", (unsigned int)count,
           *leadout_lba);
    return 0;
}

static int send_all(int fd, const uint8_t *data, size_t length)
{
    size_t sent = 0;
    while (sent < length) {
        ssize_t count = send(fd, data + sent, length - sent, 0);
        if (count < 0 && errno == EINTR)
            continue;
        if (count <= 0)
            return -1;
        sent += (size_t)count;
    }
    return 0;
}

static void put_le16(uint8_t *output, uint16_t value)
{
    output[0] = (uint8_t)value;
    output[1] = (uint8_t)(value >> 8);
}

static void put_le32(uint8_t *output, uint32_t value)
{
    output[0] = (uint8_t)value;
    output[1] = (uint8_t)(value >> 8);
    output[2] = (uint8_t)(value >> 16);
    output[3] = (uint8_t)(value >> 24);
}

static void make_wav_header(uint8_t header[44], uint32_t pcm_bytes)
{
    memset(header, 0, 44U);
    memcpy(header, "RIFF", 4U);
    put_le32(header + 4U, 36U + pcm_bytes);
    memcpy(header + 8U, "WAVEfmt ", 8U);
    put_le32(header + 16U, 16U);
    put_le16(header + 20U, 1U);
    put_le16(header + 22U, 2U);
    put_le32(header + 24U, 44100U);
    put_le32(header + 28U, 176400U);
    put_le16(header + 32U, 4U);
    put_le16(header + 34U, 16U);
    memcpy(header + 36U, "data", 4U);
    put_le32(header + 40U, pcm_bytes);
}

static int parse_decimal(const char **cursor, unsigned long *value)
{
    const char *start = *cursor;
    while (isdigit((unsigned char)**cursor))
        ++*cursor;
    if (*cursor == start)
        return -1;
    errno = 0;
    char *end = NULL;
    unsigned long parsed = strtoul(start, &end, 10);
    if (errno != 0 || end != *cursor)
        return -1;
    *value = parsed;
    return 0;
}

static int parse_track_target(const char *target, unsigned int *track_number,
                              uint32_t *requested_sectors,
                              int *has_sector_limit)
{
    static const char prefix[] = "/track/";
    static const char suffix[] = ".wav";
    static const char query[] = "?sectors=";
    if (strncmp(target, prefix, sizeof(prefix) - 1U) != 0)
        return -1;
    const char *cursor = target + sizeof(prefix) - 1U;
    unsigned long number = 0;
    if (parse_decimal(&cursor, &number) != 0 || number == 0 || number > MAX_TRACKS ||
        strncmp(cursor, suffix, sizeof(suffix) - 1U) != 0)
        return -1;
    cursor += sizeof(suffix) - 1U;
    *has_sector_limit = 0;
    *requested_sectors = 0;
    if (*cursor == '\0') {
        *track_number = (unsigned int)number;
        return 0;
    }
    if (strncmp(cursor, query, sizeof(query) - 1U) != 0)
        return -1;
    cursor += sizeof(query) - 1U;
    unsigned long sectors = 0;
    if (parse_decimal(&cursor, &sectors) != 0 || *cursor != '\0' ||
        sectors == 0 || sectors > UINT32_MAX)
        return -1;
    *track_number = (unsigned int)number;
    *requested_sectors = (uint32_t)sectors;
    *has_sector_limit = 1;
    return 0;
}

static int send_response(int fd, const char *status, const char *content_type,
                         const char *body, size_t body_size)
{
    char header[512];
    int length = snprintf(header, sizeof(header),
                          "HTTP/1.1 %s\r\n"
                          "Content-Type: %s\r\n"
                          "Content-Length: %u\r\n"
                          "Connection: close\r\n"
                          "Cache-Control: no-store\r\n\r\n",
                          status, content_type, (unsigned int)body_size);
    if (length <= 0 || (size_t)length >= sizeof(header))
        return -1;
    if (send_all(fd, (const uint8_t *)header, (size_t)length) != 0)
        return -1;
    return body_size == 0 ? 0 : send_all(fd, (const uint8_t *)body, body_size);
}

static int send_index(int fd, const struct track_info *tracks, size_t count)
{
    char body[8192];
    size_t used = 0;
    int length = snprintf(body, sizeof(body),
                          "{\"format\":\"CD-DA\",\"sample_rate\":44100,"
                          "\"channels\":2,\"bits\":16,\"tracks\":[");
    if (length <= 0 || (size_t)length >= sizeof(body))
        return -1;
    used = (size_t)length;
    int first = 1;
    for (size_t i = 0; i < count; ++i) {
        if (!tracks[i].valid || (tracks[i].control & 0x04U) != 0)
            continue;
        length = snprintf(body + used, sizeof(body) - used,
                          "%s{\"number\":%u,\"start_lba\":%u,"
                          "\"sectors\":%u,\"href\":\"/track/%u.wav\"}",
                          first ? "" : ",", (unsigned int)tracks[i].number,
                          tracks[i].start_lba,
                          tracks[i].end_lba - tracks[i].start_lba,
                          (unsigned int)tracks[i].number);
        if (length <= 0 || (size_t)length >= sizeof(body) - used)
            return -1;
        used += (size_t)length;
        first = 0;
    }
    if (used + 3U >= sizeof(body))
        return -1;
    memcpy(body + used, "]}\n", 3U);
    used += 3U;
    return send_response(fd, "200 OK", "application/json; charset=utf-8",
                         body, used);
}

static int stream_track(int fd, const struct track_info *track,
                        uint32_t requested_sectors, int has_sector_limit)
{
    uint32_t track_sectors = track->end_lba - track->start_lba;
    uint32_t sectors = has_sector_limit ? requested_sectors : track_sectors;
    if (sectors == 0 || sectors > track_sectors ||
        (uint64_t)sectors * CDDA_BYTES_PER_SECTOR > UINT32_MAX - 36U) {
        static const char body[] = "sector count outside this track\n";
        return send_response(fd, "416 Range Not Satisfiable", "text/plain",
                             body, sizeof(body) - 1U);
    }
    uint32_t pcm_bytes = sectors * CDDA_BYTES_PER_SECTOR;
    uint8_t wav_header[44];
    make_wav_header(wav_header, pcm_bytes);
    char http_header[512];
    int header_length = snprintf(http_header, sizeof(http_header),
                                 "HTTP/1.1 200 OK\r\n"
                                 "Content-Type: audio/wav\r\n"
                                 "Content-Length: %u\r\n"
                                 "Content-Disposition: inline; filename=\"track-%u.wav\"\r\n"
                                 "Connection: close\r\n"
                                 "Cache-Control: no-store\r\n\r\n",
                                 pcm_bytes + 44U, (unsigned int)track->number);
    if (header_length <= 0 || (size_t)header_length >= sizeof(http_header) ||
        send_all(fd, (const uint8_t *)http_header, (size_t)header_length) != 0 ||
        send_all(fd, wav_header, sizeof(wav_header)) != 0) {
        report("HTTP_WAV client disconnected before track=%u start", track->number);
        return -1;
    }

    report("HTTP_WAV_BEGIN track=%u start_lba=%u sectors=%u bytes=%u mode=%s",
           (unsigned int)track->number, track->start_lba, sectors, pcm_bytes,
           has_sector_limit ? "bounded-sample" : "full-track");
    uint8_t pcm[READ_BATCH_BYTES];
    uint32_t completed = 0;
    uint32_t hash = 2166136261U;
    while (completed < sectors) {
        uint32_t batch = sectors - completed;
        if (batch > READ_BATCH_SECTORS)
            batch = READ_BATCH_SECTORS;
        uint32_t lba = track->start_lba + completed;
        size_t batch_bytes = (size_t)batch * CDDA_BYTES_PER_SECTOR;
        uint8_t cdb[12] = {0xbeU, 0x04U, 0, 0, 0, 0, 0, 0, 0, 0x10U, 0, 0};
        cdb[2] = (uint8_t)(lba >> 24);
        cdb[3] = (uint8_t)(lba >> 16);
        cdb[4] = (uint8_t)(lba >> 8);
        cdb[5] = (uint8_t)lba;
        cdb[6] = (uint8_t)(batch >> 16);
        cdb[7] = (uint8_t)(batch >> 8);
        cdb[8] = (uint8_t)batch;
        if (issue_read("READ_CD_AUDIO", cdb, sizeof(cdb), pcm, batch_bytes, 0) != 0) {
            report("HTTP_WAV_READ_FAIL track=%u lba=%u sectors=%u",
                   (unsigned int)track->number, lba, batch);
            return -1;
        }
        hash = fnv1a_update(hash, pcm, batch_bytes);
        /* CD-DA main-channel samples are MSB first; RIFF/WAVE PCM is LE. */
        for (size_t i = 0; i + 1U < batch_bytes; i += 2U) {
            uint8_t temporary = pcm[i];
            pcm[i] = pcm[i + 1U];
            pcm[i + 1U] = temporary;
        }
        if (send_all(fd, pcm, batch_bytes) != 0) {
            report("HTTP_WAV_SEND_FAIL track=%u lba=%u sectors=%u errno=%d",
                   (unsigned int)track->number, lba, batch, errno);
            return -1;
        }
        completed += batch;
        if (completed == sectors || completed % 1500U == 0U)
            report("HTTP_WAV_PROGRESS track=%u sectors=%u/%u",
                   (unsigned int)track->number, completed, sectors);
    }
    report("HTTP_WAV_END result=PASS track=%u sectors=%u pcm_bytes=%u fnv1a32=%08x",
           (unsigned int)track->number, sectors, pcm_bytes, hash);
    return 0;
}

static int read_request_target(int fd, char *target, size_t capacity)
{
    char request[MAX_HTTP_REQUEST];
    size_t used = 0;
    while (used + 1U < sizeof(request)) {
        ssize_t count = recv(fd, request + used, sizeof(request) - used - 1U, 0);
        if (count < 0 && errno == EINTR)
            continue;
        if (count <= 0)
            return -1;
        used += (size_t)count;
        request[used] = '\0';
        char *line_end = strstr(request, "\r\n");
        if (line_end != NULL) {
            *line_end = '\0';
            char method[8];
            char protocol[16];
            if (sscanf(request, "%7s %255s %15s", method, target, protocol) != 3 ||
                strcmp(method, "GET") != 0 || strncmp(protocol, "HTTP/1.", 7U) != 0 ||
                strlen(target) >= capacity)
                return -1;
            return 0;
        }
    }
    return -1;
}

static int handle_client(int fd, const struct track_info *tracks, size_t count)
{
    struct timeval timeout;
    timeout.tv_sec = 10;
    timeout.tv_usec = 0;
    (void)setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO, &timeout, sizeof(timeout));
#ifdef SO_NOSIGPIPE
    int no_sigpipe = 1;
    (void)setsockopt(fd, SOL_SOCKET, SO_NOSIGPIPE, &no_sigpipe, sizeof(no_sigpipe));
#endif

    char target[256];
    if (read_request_target(fd, target, sizeof(target)) != 0) {
        static const char body[] = "bad GET request\n";
        (void)send_response(fd, "400 Bad Request", "text/plain", body,
                            sizeof(body) - 1U);
        report("HTTP_REQUEST result=BAD");
        return 0;
    }
    report("HTTP_REQUEST target=%s", target);
    if (strcmp(target, "/") == 0 || strcmp(target, "/tracks.json") == 0) {
        (void)send_index(fd, tracks, count);
        return 0;
    }
    if (strcmp(target, "/__shutdown") == 0) {
        static const char body[] = "stopped\n";
        (void)send_response(fd, "200 OK", "text/plain", body,
                            sizeof(body) - 1U);
        report("HTTP_SERVER shutdown requested");
        return 1;
    }

    unsigned int number = 0;
    uint32_t requested_sectors = 0;
    int has_sector_limit = 0;
    if (parse_track_target(target, &number, &requested_sectors,
                           &has_sector_limit) != 0) {
        static const char body[] = "use /tracks.json or /track/N.wav\n";
        (void)send_response(fd, "404 Not Found", "text/plain", body,
                            sizeof(body) - 1U);
        return 0;
    }
    for (size_t i = 0; i < count; ++i) {
        if (tracks[i].valid && tracks[i].number == number) {
            if ((tracks[i].control & 0x04U) != 0) {
                static const char body[] = "track is data, not CD-DA\n";
                (void)send_response(fd, "415 Unsupported Media Type", "text/plain",
                                    body, sizeof(body) - 1U);
                return 0;
            }
            (void)stream_track(fd, &tracks[i], requested_sectors,
                               has_sector_limit);
            return 0;
        }
    }
    static const char body[] = "track not present in TOC\n";
    (void)send_response(fd, "404 Not Found", "text/plain", body,
                        sizeof(body) - 1U);
    return 0;
}

static int serve_http(const struct track_info *tracks, size_t count)
{
    int server = socket(AF_INET, SOCK_STREAM, 0);
    if (server < 0) {
        report("HTTP_SOCKET result=FAIL errno=%d", errno);
        return -1;
    }
    int reuse = 1;
    (void)setsockopt(server, SOL_SOCKET, SO_REUSEADDR, &reuse, sizeof(reuse));
#ifdef SO_NOSIGPIPE
    int no_sigpipe = 1;
    (void)setsockopt(server, SOL_SOCKET, SO_NOSIGPIPE, &no_sigpipe, sizeof(no_sigpipe));
#endif
    struct sockaddr_in address;
    memset(&address, 0, sizeof(address));
    address.sin_len = sizeof(address);
    address.sin_family = AF_INET;
    address.sin_addr.s_addr = htonl(INADDR_ANY);
    address.sin_port = htons((uint16_t)HTTP_PORT);
    if (bind(server, (struct sockaddr *)&address, sizeof(address)) != 0 ||
        listen(server, 4) != 0) {
        report("HTTP_LISTEN result=FAIL port=%u errno=%d", HTTP_PORT, errno);
        close(server);
        return -1;
    }
    report("HTTP_LISTEN result=PASS port=%u url=http://<ps5-ip>:%u/", HTTP_PORT,
           HTTP_PORT);

    int stop = 0;
    while (!stop) {
        struct sockaddr_in peer;
        socklen_t peer_length = sizeof(peer);
        int client = accept(server, (struct sockaddr *)&peer, &peer_length);
        if (client < 0 && errno == EINTR)
            continue;
        if (client < 0) {
            report("HTTP_ACCEPT result=FAIL errno=%d", errno);
            break;
        }
        stop = handle_client(client, tracks, count);
        shutdown(client, SHUT_RDWR);
        close(client);
    }
    close(server);
    return stop ? 0 : -1;
}

int main(int argc, char **argv)
{
    (void)argc;
    (void)argv;
    report("START schema=1 epoch=%lld pid=%ld device=%s", (long long)time(NULL),
           (long)getpid(), CD_DEVICE_PATH);
    report("SAFETY no console files created/modified/deleted; no mount; SCSI read allowlist; PCM streamed from RAM");
    if (open_devices() != 0) {
        report("END result=DEVICE_OPEN_FAILED");
        if (g_pass_fd >= 0)
            close(g_pass_fd);
        if (g_cd_fd >= 0)
            close(g_cd_fd);
        return 0;
    }

    uint8_t inquiry_cdb[6] = {0x12U, 0, 0, 0, 96U, 0};
    uint8_t inquiry[96];
    memset(inquiry, 0, sizeof(inquiry));
    if (issue_read("INQUIRY", inquiry_cdb, sizeof(inquiry_cdb), inquiry,
                   sizeof(inquiry), 1) == 0)
        report("INQUIRY vendor='%.*s' product='%.*s' revision='%.*s'", 8,
               (const char *)&inquiry[8], 16, (const char *)&inquiry[16], 4,
               (const char *)&inquiry[32]);

    uint8_t ready_cdb[6] = {0};
    if (issue_read("TEST_UNIT_READY", ready_cdb, sizeof(ready_cdb), NULL, 0, 1) != 0) {
        report("END result=MEDIA_NOT_READY");
        close(g_pass_fd);
        close(g_cd_fd);
        return 0;
    }

    struct track_info tracks[MAX_TRACKS];
    memset(tracks, 0, sizeof(tracks));
    size_t track_count = 0;
    uint32_t leadout = 0;
    if (read_toc(tracks, &track_count, &leadout) != 0) {
        report("END result=TOC_UNAVAILABLE");
        close(g_pass_fd);
        close(g_cd_fd);
        return 0;
    }
    (void)leadout;

    int result = serve_http(tracks, track_count);
    report("HTTP_SERVER result=%s", result == 0 ? "STOPPED" : "FAILED");
    close(g_pass_fd);
    close(g_cd_fd);
    report("END result=PROBE_COMPLETE");
    return 0;
}
