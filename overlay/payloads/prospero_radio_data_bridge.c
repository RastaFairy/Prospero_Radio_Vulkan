/* Prospero Radio - privileged data and AUX bridge launched by elfldr.
 * Copyright (C) 2026 BlackBearReloaded
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#include <arpa/inet.h>
#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <netinet/in.h>
#include <pthread.h>
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
#include <sys/mount.h>
#include <sys/stat.h>
#include <sys/time.h>
#include <sys/types.h>
#include <sys/uio.h>
#include <time.h>
#include <unistd.h>

#include <cam/cam.h>
#include <cam/cam_ccb.h>
#include <cam/scsi/scsi_all.h>
#include <cam/scsi/scsi_pass.h>

#include "../include/radio_disc_protocol.h"
#include "../include/radio_usb_protocol.h"
#include "prospero_udf_ro_reader.h"

#define BRIDGE_NAME "ProsperoRadioDataBridge.elf"
#define DATA_DIRECTORY "/data/radio"
#define REPORT_PATH DATA_DIRECTORY "/prospero-payload-probe.log"
#define AUX_LIST_PATH DATA_DIRECTORY "/radio-aux.m3u"
#define BRIDGE_MAX_FILE (64U * 1024U * 1024U)
#define BRIDGE_CHUNK 32768U
#define AUX_MAX_BODY (4U * 1024U * 1024U)
#define AUX_ENVELOPE_HEADER_BYTES 9U
#define AUX_MAX_STORAGE (AUX_MAX_BODY + AUX_ENVELOPE_HEADER_BYTES)
#define AUX_FORMAT_AUTO 0U
#define AUX_FORMAT_M3U 1U
#define AUX_FORMAT_M3U8 2U
#define AUX_FORMAT_PLS 3U
#define AUX_FORMAT_XSPF 4U
#define AUX_FORMAT_ASX 5U
#define RPC_PORT 7001
#define AUX_PORT 7000
#define DISC_PORT RADIO_DISC_HTTP_PORT
#define BRIDGE_IDLE_SECONDS 120
#define DISC_CDB_TIMEOUT_MS 30000U
#define DISC_AUDIO_BYTES_PER_SECTOR 2352U
#define DISC_DATA_BYTES_PER_SECTOR 2048U
#define DISC_READ_BATCH_SECTORS 20U
#define DISC_WORKER_STACK_BYTES (512U * 1024U)
#define DISC_TOC_BYTES 804U
#define DISC_MAX_SCAN_DEPTH 5U
#define DISC_MAX_SCAN_NODES 2048U
#define DISC_PATH_BYTES 512U
#define DISC_HTTP_QUEUE_CAPACITY 2U
#define AUX_HTTP_QUEUE_CAPACITY 2U
#define USB_MAX_MOUNT_ROOTS 8U
#define USB_MAX_DEVICE_CANDIDATES 32U

enum bridge_operation
{
    BRIDGE_PING = 1,
    BRIDGE_GET = 2,
    BRIDGE_PUT = 3,
    BRIDGE_STOP = 4,
    BRIDGE_STATUS = 5,
    BRIDGE_START_AUX = 6,
    BRIDGE_STOP_AUX = 7,
    BRIDGE_ATTACH = 8,
    BRIDGE_GET_AUX_BUFFER = 9,
    BRIDGE_PUT_AUX_BUFFER = 10,
    BRIDGE_GET_DISC_INDEX = 11,
    BRIDGE_GET_USB_INDEX = 12
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
static int g_aux_start_failed;
static pthread_mutex_t g_aux_worker_mutex = PTHREAD_MUTEX_INITIALIZER;
static pthread_cond_t g_aux_worker_condition = PTHREAD_COND_INITIALIZER;
static pthread_t g_aux_worker_thread;
static int g_aux_worker_started;
static int g_aux_worker_shutdown;
static int g_aux_http_queue[AUX_HTTP_QUEUE_CAPACITY];
static size_t g_aux_http_queue_head;
static size_t g_aux_http_queue_count;
static time_t g_aux_queue_full_log_after;
static int g_parent_pid = -1;
static struct disc_track_info
{
    unsigned char number;
    unsigned char control;
    uint32_t start_lba;
    uint32_t end_lba;
    int valid;
} g_disc_tracks[100];
static size_t g_disc_track_count;
enum disc_audio_source_kind
{
    DISC_AUDIO_SOURCE_MOUNTED = 1,
    DISC_AUDIO_SOURCE_UDF_RAW = 2
};
struct disc_audio_source
{
    unsigned char type;
    unsigned char source_kind;
    union
    {
        char mounted_path[DISC_PATH_BYTES];
        struct radio_udf_ro_file udf_file;
    } location;
};
static struct disc_audio_source g_disc_audio_sources[RADIO_DISC_MAX_ENTRIES];
static unsigned g_disc_audio_file_count;
static unsigned g_disc_entry_count;
static unsigned g_disc_media_kind;
static unsigned g_disc_scan_nodes;
static int g_disc_mount_owned;
static const char *g_disc_device_path;
static uint32_t g_disc_first_data_lba;
static uint32_t g_disc_capacity_last_lba;
static int g_disc_capacity_valid;
static struct radio_udf_ro_reader g_disc_udf_reader;
enum disc_stream_kind
{
    DISC_STREAM_NONE = 0,
    DISC_STREAM_AUDIO = 1,
    DISC_STREAM_FILE = 2
};
static struct disc_stream_state
{
    int client;
    int file_fd;
    enum disc_stream_kind kind;
    const struct disc_track_info *track;
    unsigned char file_type;
    uint32_t total_sectors;
    uint32_t completed_sectors;
    uint64_t file_remaining;
    struct radio_udf_ro_file_reader udf_file_reader;
} g_disc_stream = {
    .client = -1,
    .file_fd = -1,
    .kind = DISC_STREAM_NONE
};
static pthread_mutex_t g_disc_worker_mutex = PTHREAD_MUTEX_INITIALIZER;
static pthread_cond_t g_disc_worker_condition = PTHREAD_COND_INITIALIZER;
static pthread_t g_disc_worker_thread;
static int g_disc_worker_started;
static int g_disc_worker_shutdown;
static int g_disc_scan_requested;
static int g_disc_scan_active;
static int g_disc_index_cache_valid;
static unsigned char g_disc_index_cache[RADIO_DISC_INDEX_MAX_BYTES];
static size_t g_disc_index_cache_size;
static int g_disc_http_queue[DISC_HTTP_QUEUE_CAPACITY];
static size_t g_disc_http_queue_head;
static size_t g_disc_http_queue_count;
/* Build each USB index away from the live HTTP path table, then publish it
 * atomically so a rescan cannot change paths during playback. */
static char g_usb_paths[RADIO_USB_MAX_ENTRIES][RADIO_USB_PATH_BYTES];
static unsigned char g_usb_types[RADIO_USB_MAX_ENTRIES];
static unsigned g_usb_file_count;
static int g_usb_device_available;
static char g_usb_scan_paths[RADIO_USB_MAX_ENTRIES][RADIO_USB_PATH_BYTES];
static unsigned char g_usb_scan_types[RADIO_USB_MAX_ENTRIES];
static unsigned g_usb_scan_file_count;
static unsigned g_usb_scan_nodes;
static int g_usb_scan_device_available;
static char g_usb_owned_mounts[USB_MAX_MOUNT_ROOTS][RADIO_USB_PATH_BYTES];
static char g_usb_owned_sources[USB_MAX_MOUNT_ROOTS][RADIO_USB_PATH_BYTES];
static unsigned g_usb_owned_mount_count;
static pthread_mutex_t g_usb_mutex = PTHREAD_MUTEX_INITIALIZER;
static pthread_t g_usb_thread;
static int g_usb_thread_started;
static int g_usb_shutdown;
static int g_usb_scan_requested;
static int g_usb_scan_active;
static int g_usb_cache_valid;
static unsigned char g_usb_index_cache[RADIO_USB_INDEX_MAX_BYTES];
static size_t g_usb_index_cache_size;

static int serve_usb_index(int connection, int force_refresh);
static int usb_worker_start(void);
static void usb_worker_stop(void);
static int aux_worker_start(void);
static void aux_worker_stop(void);
static int aux_worker_enqueue_http(int connection);

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

static int serve_get(int connection, unsigned char file_id, uint32_t maximum_size)
{
    char path[192];
    struct stat info;
    if (file_path(file_id, path, sizeof(path)) != 0)
        return send_response(connection, BRIDGE_BAD_REQUEST, 0);
    int fd = open(path, O_RDONLY);
    if (fd < 0)
        return send_response(connection, errno == ENOENT ? BRIDGE_NOT_FOUND : BRIDGE_IO_ERROR, 0);
    if (fstat(fd, &info) != 0 || !S_ISREG(info.st_mode) || info.st_size < 0 ||
        (uint64_t)info.st_size > maximum_size)
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
    if (file_path(file_id, path, sizeof(path)) != 0 || size > BRIDGE_MAX_FILE ||
        (file_id == 6U && size > AUX_MAX_STORAGE))
        return send_response(connection, BRIDGE_BAD_REQUEST, 0);
    int path_length = file_id == 6U
                          ? snprintf(temporary, sizeof(temporary), "%s.rpc.tmp", path)
                          : snprintf(temporary, sizeof(temporary), "%s.tmp", path);
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

static int g_disc_pass_fd = -1;
static path_id_t g_disc_path_id;
static target_id_t g_disc_target_id;
static lun_id_t g_disc_target_lun;

static uint16_t disc_read_u16be(const unsigned char *data)
{
    return (uint16_t)(((uint16_t)data[0] << 8) | data[1]);
}

static uint32_t disc_read_u32be(const unsigned char *data)
{
    return ((uint32_t)data[0] << 24) | ((uint32_t)data[1] << 16) |
           ((uint32_t)data[2] << 8) | (uint32_t)data[3];
}

static void disc_write_u16be(unsigned char *data, unsigned value)
{
    data[0] = (unsigned char)(value >> 8);
    data[1] = (unsigned char)value;
}

static void disc_write_u32be(unsigned char *data, uint32_t value)
{
    data[0] = (unsigned char)(value >> 24);
    data[1] = (unsigned char)(value >> 16);
    data[2] = (unsigned char)(value >> 8);
    data[3] = (unsigned char)value;
}

static int disc_issue_read(const unsigned char *cdb, unsigned char cdb_length,
                           void *buffer, size_t size, uint32_t *transferred)
{
    if (g_disc_pass_fd < 0 || cdb == NULL || cdb_length == 0 ||
        cdb_length > CAM_MAX_CDBLEN || size > UINT32_MAX)
        return -1;
    switch (cdb[0])
    {
    case 0x00U: /* TEST UNIT READY */
        if (size != 0U)
            return -1;
        break;
    case 0x25U: /* READ CAPACITY(10) */
        if (cdb_length != 10U || size != 8U)
            return -1;
        break;
    case 0x28U: /* READ(10), one block for metadata or bounded batches */
    {
        const uint32_t blocks = ((uint32_t)cdb[7] << 8) | cdb[8];
        if (cdb_length != 10U || blocks == 0U || blocks > DISC_READ_BATCH_SECTORS ||
            size != (size_t)blocks * DISC_DATA_BYTES_PER_SECTOR)
            return -1;
        break;
    }
    case 0x43U: /* READ TOC/PMA/ATIP */
    case 0xbeU: /* READ CD */
        break;
    default:
        return -1;
    }
    union ccb ccb;
    memset(&ccb, 0, sizeof(ccb));
    ccb.ccb_h.path_id = g_disc_path_id;
    ccb.ccb_h.target_id = g_disc_target_id;
    ccb.ccb_h.target_lun = g_disc_target_lun;
    ccb.ccb_h.flags = size == 0U ? CAM_DIR_NONE : CAM_DIR_IN;
    ccb.ccb_h.flags |= CAM_DEV_QFRZDIS;
    memcpy(ccb.csio.cdb_io.cdb_bytes, cdb, cdb_length);
    cam_fill_csio(&ccb.csio, 0, NULL, ccb.ccb_h.flags, CAM_TAG_ACTION_NONE,
                  buffer, (uint32_t)size, (uint8_t)sizeof(ccb.csio.sense_data),
                  cdb_length, DISC_CDB_TIMEOUT_MS);
    ccb.ccb_h.path_id = g_disc_path_id;
    ccb.ccb_h.target_id = g_disc_target_id;
    ccb.ccb_h.target_lun = g_disc_target_lun;
    const int result = ioctl(g_disc_pass_fd, CAMIOCOMMAND, &ccb);
    const uint32_t cam_status = ccb.ccb_h.status & CAM_STATUS_MASK;
    const uint32_t amount = ccb.csio.resid <= size ? (uint32_t)(size - ccb.csio.resid) : 0U;
    if (transferred != NULL)
        *transferred = amount;
    if (result != 0 || cam_status != CAM_REQ_CMP || ccb.csio.scsi_status != 0 ||
        ((cdb[0] == 0xbeU || cdb[0] == 0x25U || cdb[0] == 0x28U) &&
         ccb.csio.resid != 0U) ||
        (cdb[0] == 0x43U && amount < 4U))
    {
        unsigned char sense[18];
        size_t sense_size = ccb.csio.sense_len;
        if (sense_size > sizeof(sense))
            sense_size = sizeof(sense);
        memcpy(sense, &ccb.csio.sense_data, sense_size);
        if (sense_size >= 14U && (sense[0] & 0x7eU) == 0x70U)
            report("CD read command failed opcode=%02x errno=%d cam=%#x scsi=%#x key=%#x asc=%#x ascq=%#x",
                   cdb[0], result == 0 ? 0 : errno, cam_status,
                   (unsigned)ccb.csio.scsi_status, (unsigned)(sense[2] & 0x0fU),
                   (unsigned)sense[12], (unsigned)sense[13]);
        else
            report("CD read command failed opcode=%02x errno=%d cam=%#x scsi=%#x resid=%u",
                   cdb[0], result == 0 ? 0 : errno, cam_status,
                   (unsigned)ccb.csio.scsi_status, (unsigned)ccb.csio.resid);
        return -1;
    }
    return 0;
}

static int disc_read_capacity(void)
{
    unsigned char cdb[10] = {0x25U, 0U, 0U, 0U, 0U, 0U, 0U, 0U, 0U, 0U};
    unsigned char response[8];
    memset(response, 0, sizeof(response));
    g_disc_capacity_valid = 0;
    if (disc_issue_read(cdb, sizeof(cdb), response, sizeof(response), NULL) != 0)
        return -1;
    const uint32_t last_lba = ((uint32_t)response[0] << 24) |
                              ((uint32_t)response[1] << 16) |
                              ((uint32_t)response[2] << 8) | response[3];
    const uint32_t block_size = ((uint32_t)response[4] << 24) |
                                ((uint32_t)response[5] << 16) |
                                ((uint32_t)response[6] << 8) | response[7];
    if (block_size != DISC_DATA_BYTES_PER_SECTOR || last_lba == 0U)
    {
        report("optical READ CAPACITY unsupported last_lba=%u block_size=%u",
               last_lba, block_size);
        return -1;
    }
    g_disc_capacity_last_lba = last_lba;
    g_disc_capacity_valid = 1;
    report("optical READ CAPACITY last_lba=%u block_size=%u",
           last_lba, block_size);
    return 0;
}

static int disc_read_data_blocks(void *opaque, uint32_t lba, uint16_t blocks,
                                 uint8_t *buffer)
{
    (void)opaque;
    if (!g_disc_capacity_valid || blocks == 0U ||
        (uint64_t)lba + blocks - 1U > g_disc_capacity_last_lba)
        return -1;
    unsigned char cdb[10] = {0x28U, 0U, 0U, 0U, 0U, 0U, 0U, 0U, 0U, 0U};
    cdb[2] = (unsigned char)(lba >> 24);
    cdb[3] = (unsigned char)(lba >> 16);
    cdb[4] = (unsigned char)(lba >> 8);
    cdb[5] = (unsigned char)lba;
    cdb[7] = (unsigned char)(blocks >> 8);
    cdb[8] = (unsigned char)blocks;
    return disc_issue_read(cdb, sizeof(cdb), buffer,
                           (size_t)blocks * DISC_DATA_BYTES_PER_SECTOR, NULL);
}

static int disc_open_pass(void)
{
    if (g_disc_pass_fd >= 0)
    {
        close(g_disc_pass_fd);
        g_disc_pass_fd = -1;
    }
    g_disc_device_path = NULL;
    const char *const candidates[] = {RADIO_DISC_DEVICE_PATH};
    for (size_t candidate = 0U;
         candidate < sizeof(candidates) / sizeof(candidates[0]); ++candidate)
    {
        int cd_fd = open(candidates[candidate], O_RDONLY);
        if (cd_fd < 0)
            continue;
        union ccb ccb;
        memset(&ccb, 0, sizeof(ccb));
        ccb.ccb_h.func_code = XPT_GDEVLIST;
        if (ioctl(cd_fd, CAMGETPASSTHRU, &ccb) != 0 ||
            ccb.cgdl.status == CAM_GDEVLIST_ERROR)
        {
            close(cd_fd);
            continue;
        }
        char pass_path[64];
        const int path_size = snprintf(pass_path, sizeof(pass_path), "/dev/%.*s%u",
                                       DEV_IDLEN, ccb.cgdl.periph_name,
                                       (unsigned)ccb.cgdl.unit_number);
        close(cd_fd);
        if (path_size <= 0 || (size_t)path_size >= sizeof(pass_path))
            continue;
        /* pass(4) requires O_RDWR; the allowlist below permits only TUR,
         * READ CAPACITY, READ(10), READ TOC and READ CD commands. */
        int pass_fd = open(pass_path, O_RDWR);
        if (pass_fd < 0)
            continue;
        memset(&ccb, 0, sizeof(ccb));
        ccb.ccb_h.func_code = XPT_GDEVLIST;
        if (ioctl(pass_fd, CAMGETPASSTHRU, &ccb) != 0 ||
            ccb.cgdl.status == CAM_GDEVLIST_ERROR)
        {
            close(pass_fd);
            continue;
        }
        g_disc_path_id = ccb.ccb_h.path_id;
        g_disc_target_id = ccb.ccb_h.target_id;
        g_disc_target_lun = ccb.ccb_h.target_lun;
        g_disc_pass_fd = pass_fd;
        g_disc_device_path = candidates[candidate];
        const unsigned char ready_cdb[6] = {0x00U, 0U, 0U, 0U, 0U, 0U};
        if (disc_issue_read(ready_cdb, sizeof(ready_cdb), NULL, 0U, NULL) == 0)
        {
            report("optical device selected path=%s", g_disc_device_path);
            return 0;
        }
        report("optical device has no ready medium path=%s", g_disc_device_path);
        close(g_disc_pass_fd);
        g_disc_pass_fd = -1;
        g_disc_device_path = NULL;
    }
    return -1;
}

static int disc_read_toc(int *has_data_track)
{
    g_disc_first_data_lba = 0U;
    unsigned char cdb[10] = {0x43U, 0, 0, 0, 0, 0, 0, 0, 0, 0};
    unsigned char data[DISC_TOC_BYTES];
    memset(data, 0, sizeof(data));
    cdb[7] = (unsigned char)(sizeof(data) >> 8);
    cdb[8] = (unsigned char)sizeof(data);
    uint32_t transferred = 0U;
    if (disc_issue_read(cdb, sizeof(cdb), data, sizeof(data), &transferred) != 0)
        return -1;
    const unsigned response_length = disc_read_u16be(data);
    const unsigned first = data[2];
    const unsigned last = data[3];
    if (response_length < 4U || first == 0U || last < first ||
        last >= 100U || response_length + 2U > transferred)
        return -1;
    const unsigned track_count = last - first + 1U;
    memset(g_disc_tracks, 0, sizeof(g_disc_tracks));
    uint32_t leadout = 0U;
    const unsigned descriptor_count = response_length / 8U;
    for (unsigned i = 0U; i < descriptor_count; ++i)
    {
        const unsigned char *entry = data + 4U + i * 8U;
        const unsigned number = entry[2];
        if (number == 0xaaU)
            leadout = disc_read_u32be(entry + 4U);
        else if (number >= first && number <= last)
        {
            struct disc_track_info *track = &g_disc_tracks[number];
            track->number = (unsigned char)number;
            track->control = (unsigned char)(entry[1] & 0x0fU);
            track->start_lba = disc_read_u32be(entry + 4U);
            track->valid = 1;
            if ((track->control & 0x04U) != 0U)
            {
                *has_data_track = 1;
                if (g_disc_first_data_lba == 0U)
                    g_disc_first_data_lba = track->start_lba;
            }
        }
    }
    if (leadout == 0U)
        return -1;
    for (unsigned number = first; number <= last; ++number)
    {
        struct disc_track_info *track = &g_disc_tracks[number];
        if (!track->valid)
            continue;
        uint32_t end = leadout;
        for (unsigned next = number + 1U; next <= last; ++next)
        {
            if (g_disc_tracks[next].valid)
            {
                end = g_disc_tracks[next].start_lba;
                break;
            }
        }
        if (end <= track->start_lba)
            return -1;
        track->end_lba = end;
        if ((track->control & 0x04U) == 0U)
            ++g_disc_track_count;
    }
    (void)track_count;
    return 0;
}

static int disc_append_index_entry(unsigned char *index, size_t capacity, size_t *used,
                                   unsigned char type, unsigned id,
                                   unsigned char track_number, uint32_t start_lba,
                                   uint32_t end_lba, const char *name)
{
    if (g_disc_entry_count >= RADIO_DISC_MAX_ENTRIES || name == NULL)
        return -1;
    const size_t name_size = strlen(name);
    if (name_size > RADIO_DISC_NAME_BYTES || *used > capacity ||
        RADIO_DISC_INDEX_RECORD_FIXED_BYTES + name_size > capacity - *used)
        return -1;
    unsigned char *record = index + *used;
    record[0] = type;
    disc_write_u16be(record + 1U, id);
    record[3] = track_number;
    disc_write_u32be(record + 4U, start_lba);
    disc_write_u32be(record + 8U, end_lba);
    disc_write_u16be(record + 12U, (unsigned)name_size);
    memcpy(record + RADIO_DISC_INDEX_RECORD_FIXED_BYTES, name, name_size);
    *used += RADIO_DISC_INDEX_RECORD_FIXED_BYTES + name_size;
    ++g_disc_entry_count;
    return 0;
}

static int disc_mount_matches_source(const struct statfs *info)
{
    return info != NULL && g_disc_device_path != NULL &&
           strcmp(info->f_mntonname, RADIO_DISC_MOUNT_PATH) == 0 &&
           strcmp(info->f_mntfromname, g_disc_device_path) == 0 &&
           (info->f_flags & MNT_RDONLY) != 0 &&
           (strcmp(info->f_fstypename, "cd9660") == 0 ||
            strcmp(info->f_fstypename, "udf2") == 0 ||
            strcmp(info->f_fstypename, "udf") == 0);
}

static int disc_mount_readonly(void)
{
    struct statfs info;
    if (statfs(RADIO_DISC_MOUNT_PATH, &info) != 0)
    {
        report("optical mount skipped: statfs %s errno=%d",
               RADIO_DISC_MOUNT_PATH, errno);
        return -1;
    }
    if (disc_mount_matches_source(&info))
        return 0;
    if (strcmp(info.f_mntonname, "/mnt") != 0 ||
        strcmp(info.f_fstypename, "tmpfs") != 0)
    {
        report("CD-MP3 mount refused: mountpoint is not the verified /mnt tmpfs base");
        return -1;
    }
    static const char *const filesystem_types[] = {"cd9660", "udf2", "udf"};
    for (size_t attempt = 0U;
         attempt < sizeof(filesystem_types) / sizeof(filesystem_types[0]); ++attempt)
    {
        struct iovec options[6];
        const char *names[3] = {"fstype", "fspath", "from"};
        if (g_disc_device_path == NULL)
            return -1;
        const char *values[3] = {filesystem_types[attempt], RADIO_DISC_MOUNT_PATH,
                                 g_disc_device_path};
        for (size_t index = 0U; index < 3U; ++index)
        {
            options[index * 2U].iov_base = (void *)names[index];
            options[index * 2U].iov_len = strlen(names[index]) + 1U;
            options[index * 2U + 1U].iov_base = (void *)values[index];
            options[index * 2U + 1U].iov_len = strlen(values[index]) + 1U;
        }
        if (nmount(options, 6U, MNT_RDONLY) != 0)
        {
            report("optical read-only mount type=%s failed errno=%d",
                   filesystem_types[attempt], errno);
            continue;
        }
        if (statfs(RADIO_DISC_MOUNT_PATH, &info) == 0 &&
            disc_mount_matches_source(&info) &&
            strcmp(info.f_fstypename, filesystem_types[attempt]) == 0)
        {
            g_disc_mount_owned = 1;
            report("optical data mounted type=%s source=%s path=%s readonly=yes",
                   info.f_fstypename, g_disc_device_path, RADIO_DISC_MOUNT_PATH);
            return 0;
        }
        report("optical mount type=%s verification failed; leaving unverified mount untouched",
               filesystem_types[attempt]);
        return -1;
    }
    return -1;
}

static unsigned char disc_audio_file_type(const char *name)
{
    const char *dot = strrchr(name, '.');
    if (dot == NULL)
        return 0U;
    if (strcasecmp(dot, ".mp3") == 0)
        return RADIO_DISC_ENTRY_MP3;
    if (strcasecmp(dot, ".aac") == 0 || strcasecmp(dot, ".adts") == 0)
        return RADIO_DISC_ENTRY_AAC;
    if (strcasecmp(dot, ".flac") == 0)
        return RADIO_DISC_ENTRY_FLAC;
    if (strcasecmp(dot, ".wav") == 0)
        return RADIO_DISC_ENTRY_WAV;
    return 0U;
}

static void disc_display_name(const char *source, char *output, size_t capacity)
{
    size_t used = 0U;
    while (*source != '\0' && used + 1U < capacity)
    {
        const unsigned char byte = (unsigned char)*source++;
        output[used++] = byte >= 0x20U && byte < 0x7fU ? (char)byte : '?';
    }
    output[used] = '\0';
    if (used == 0U)
        (void)snprintf(output, capacity, "Pista MP3");
}

static void disc_scan_directory(const char *path, unsigned depth,
                                unsigned char *index, size_t capacity, size_t *used)
{
    if (depth > DISC_MAX_SCAN_DEPTH || g_disc_scan_nodes >= DISC_MAX_SCAN_NODES ||
        g_disc_entry_count >= RADIO_DISC_MAX_ENTRIES)
        return;
    DIR *directory = opendir(path);
    if (directory == NULL)
        return;
    struct dirent *entry;
    while (g_disc_scan_nodes < DISC_MAX_SCAN_NODES &&
           g_disc_entry_count < RADIO_DISC_MAX_ENTRIES &&
           (entry = readdir(directory)) != NULL)
    {
        if (strcmp(entry->d_name, ".") == 0 || strcmp(entry->d_name, "..") == 0)
            continue;
        ++g_disc_scan_nodes;
        char child[DISC_PATH_BYTES];
        const int length = snprintf(child, sizeof(child), "%s/%.*s", path,
                                    (int)RADIO_DISC_NAME_BYTES, entry->d_name);
        if (length <= 0 || (size_t)length >= sizeof(child))
            continue;
        struct stat info;
        if (lstat(child, &info) != 0)
            continue;
        if (S_ISDIR(info.st_mode))
            disc_scan_directory(child, depth + 1U, index, capacity, used);
        else if (S_ISREG(info.st_mode) && disc_audio_file_type(entry->d_name) != 0U &&
                 g_disc_audio_file_count < RADIO_DISC_MAX_ENTRIES)
        {
            const unsigned id = g_disc_audio_file_count;
            const unsigned char type = disc_audio_file_type(entry->d_name);
            char display[RADIO_DISC_NAME_BYTES + 1U];
            disc_display_name(entry->d_name, display, sizeof(display));
            if (disc_append_index_entry(index, capacity, used, type,
                                        id, 0U, 0U, 0U, display) != 0)
                break;
            struct disc_audio_source *source = &g_disc_audio_sources[id];
            memset(source, 0, sizeof(*source));
            source->type = type;
            source->source_kind = DISC_AUDIO_SOURCE_MOUNTED;
            memcpy(source->location.mounted_path, child, (size_t)length + 1U);
            ++g_disc_audio_file_count;
        }
    }
    closedir(directory);
}

static void disc_publish_udf_files(unsigned char *index, size_t capacity,
                                   size_t *used)
{
    for (size_t i = 0U; i < g_disc_udf_reader.file_count; ++i)
    {
        if (g_disc_audio_file_count >= RADIO_DISC_MAX_ENTRIES ||
            g_disc_entry_count >= RADIO_DISC_MAX_ENTRIES)
            break;
        const struct radio_udf_ro_file *file = &g_disc_udf_reader.files[i];
        const unsigned id = g_disc_audio_file_count;
        if (disc_append_index_entry(index, capacity, used, file->audio_type,
                id, 0U, 0U, 0U, file->name) != 0)
            break;
        struct disc_audio_source *source = &g_disc_audio_sources[id];
        memset(source, 0, sizeof(*source));
        source->type = file->audio_type;
        source->source_kind = DISC_AUDIO_SOURCE_UDF_RAW;
        source->location.udf_file = *file;
        ++g_disc_audio_file_count;
    }
}

static int disc_media_is_mounted(void)
{
    struct statfs info;
    return statfs(RADIO_DISC_MOUNT_PATH, &info) == 0 &&
           disc_mount_matches_source(&info);
}

static int disc_device_available(void)
{
    const char *const candidates[] = {RADIO_DISC_DEVICE_PATH};
    for (size_t candidate = 0U;
         candidate < sizeof(candidates) / sizeof(candidates[0]); ++candidate)
    {
        struct stat info;
        if (stat(candidates[candidate], &info) == 0 &&
            (S_ISCHR(info.st_mode) || S_ISBLK(info.st_mode)))
            return 1;
    }
    return disc_media_is_mounted();
}

static int disc_refresh_index(unsigned char *index, size_t capacity, size_t *index_size)
{
    if (capacity < RADIO_DISC_INDEX_HEADER_BYTES || index_size == NULL)
        return -1;
    if (g_disc_mount_owned)
    {
        struct statfs mounted;
        if (statfs(RADIO_DISC_MOUNT_PATH, &mounted) != 0)
            report("CD-MP3 previous mount identity unavailable errno=%d", errno);
        else if (disc_mount_matches_source(&mounted))
        {
            if (unmount(RADIO_DISC_MOUNT_PATH, 0) == 0)
            {
                report("CD-MP3 previous read-only mount released for rescan");
                g_disc_mount_owned = 0;
            }
            else
                report("CD-MP3 previous mount could not be released errno=%d", errno);
        }
        else
            g_disc_mount_owned = 0;
    }
    const int mounted_media = disc_media_is_mounted();
    radio_udf_ro_close(&g_disc_udf_reader);
    g_disc_track_count = 0U;
    g_disc_audio_file_count = 0U;
    g_disc_entry_count = 0U;
    g_disc_media_kind = 0U;
    g_disc_scan_nodes = 0U;
    g_disc_capacity_valid = 0;
    g_disc_first_data_lba = 0U;
    memset(g_disc_audio_sources, 0, sizeof(g_disc_audio_sources));
    memset(g_disc_tracks, 0, sizeof(g_disc_tracks));
    memset(index, 0, RADIO_DISC_INDEX_HEADER_BYTES);
    memcpy(index, "PRCD", 4U);
    index[4] = RADIO_DISC_INDEX_VERSION;
    index[RADIO_DISC_HEADER_DEVICE_AVAILABLE_OFFSET] =
        (unsigned char)disc_device_available();
    size_t used = RADIO_DISC_INDEX_HEADER_BYTES;

    int has_data_track = 0;
    int data_scan_ok = 0;
    if (disc_open_pass() == 0)
    {
        if (disc_read_toc(&has_data_track) == 0)
        {
            for (unsigned number = 1U; number < 100U; ++number)
            {
                const struct disc_track_info *track = &g_disc_tracks[number];
                if (!track->valid || (track->control & 0x04U) != 0U)
                    continue;
                char name[RADIO_DISC_NAME_BYTES + 1U];
                (void)snprintf(name, sizeof(name), "CD Audio Track %02u", number);
                if (disc_append_index_entry(index, capacity, &used,
                                            RADIO_DISC_ENTRY_CDDA, number,
                                            (unsigned char)number, track->start_lba,
                                            track->end_lba, name) != 0)
                    break;
            }
            if (g_disc_entry_count > 0U)
                g_disc_media_kind = has_data_track ? 3U : 1U;
        }
        else
        {
            has_data_track = 1; /* A data-only disc may reject audio TOC handling. */
        }
    }
    if (has_data_track && g_disc_pass_fd >= 0)
        (void)disc_read_capacity();
    if (has_data_track || mounted_media)
    {
        if (disc_mount_readonly() == 0)
        {
            g_disc_media_kind = g_disc_media_kind == 1U ? 3U : 2U;
            disc_scan_directory(RADIO_DISC_MOUNT_PATH, 0U, index, capacity, &used);
            data_scan_ok = 1;
        }
        else if (has_data_track && g_disc_pass_fd >= 0 && g_disc_capacity_valid)
        {
            if (radio_udf_ro_scan(&g_disc_udf_reader, disc_read_data_blocks, NULL,
                                  g_disc_capacity_last_lba,
                                  g_disc_first_data_lba) == 0)
            {
                g_disc_media_kind = g_disc_track_count > 0U ? 3U : 2U;
                disc_publish_udf_files(index, capacity, &used);
                data_scan_ok = 1;
                report("optical raw UDF scan result=PASS directories=%u entries=%u files=%u playable=%u",
                       (unsigned)g_disc_udf_reader.directories_seen,
                       (unsigned)g_disc_udf_reader.entries_seen,
                       (unsigned)g_disc_udf_reader.files_seen,
                       (unsigned)g_disc_udf_reader.file_count);
            }
            else
                report("optical raw UDF scan result=FAIL capacity_last_lba=%u data_track_start=%u",
                       g_disc_capacity_last_lba, g_disc_first_data_lba);
        }
    }
    if (has_data_track && g_disc_track_count == 0U && !data_scan_ok)
    {
        report("optical data-disc index failed: no verified mount or raw UDF fileset");
        return -1;
    }
    if (g_disc_media_kind == 0U && g_disc_pass_fd >= 0)
    {
        close(g_disc_pass_fd);
        g_disc_pass_fd = -1;
    }
    index[5] = (unsigned char)g_disc_media_kind;
    disc_write_u16be(index + 6U, g_disc_entry_count);
    index[RADIO_DISC_HEADER_DEVICE_AVAILABLE_OFFSET] =
        (unsigned char)disc_device_available();
    *index_size = used;
    report("optical disc scan device=%s kind=%u audio_tracks=%u data_audio_files=%u entries=%u nodes=%u",
           g_disc_device_path != NULL ? g_disc_device_path : "none",
           g_disc_media_kind, (unsigned)g_disc_track_count, g_disc_audio_file_count,
           g_disc_entry_count, g_disc_scan_nodes);
    return 0;
}

static void disc_unmount_owned_media(void)
{
    if (g_disc_mount_owned)
    {
        struct statfs mounted;
        if (statfs(RADIO_DISC_MOUNT_PATH, &mounted) != 0)
            report("optical read-only mount identity unavailable errno=%d", errno);
        else if (disc_mount_matches_source(&mounted))
        {
            if (unmount(RADIO_DISC_MOUNT_PATH, 0) == 0)
            {
                report("optical read-only mount released path=%s", RADIO_DISC_MOUNT_PATH);
                g_disc_mount_owned = 0;
            }
            else
                report("optical read-only unmount failed errno=%d", errno);
        }
        else
        {
            report("CD-MP3 unmount skipped: mount identity is no longer verified");
            g_disc_mount_owned = 0;
        }
    }
    if (g_disc_pass_fd >= 0)
    {
        close(g_disc_pass_fd);
        g_disc_pass_fd = -1;
    }
    radio_udf_ro_close(&g_disc_udf_reader);
}

static int disc_http_error(int connection, const char *status, const char *message)
{
    const size_t length = strlen(message);
    char header[256];
    const int header_size = snprintf(header, sizeof(header),
                                     "HTTP/1.1 %s\r\nContent-Type: text/plain; charset=utf-8\r\n"
                                     "Content-Length: %lu\r\nConnection: close\r\n\r\n",
                                     status, (unsigned long)length);
    if (header_size <= 0 || (size_t)header_size >= sizeof(header) ||
        send_all(connection, header, (size_t)header_size) != 0)
        return -1;
    return send_all(connection, message, length);
}

static void disc_close_stream(void)
{
    if (g_disc_stream.file_fd >= 0)
        close(g_disc_stream.file_fd);
    if (g_disc_stream.client >= 0)
        close(g_disc_stream.client);
    g_disc_stream.client = -1;
    g_disc_stream.file_fd = -1;
    g_disc_stream.kind = DISC_STREAM_NONE;
    g_disc_stream.track = NULL;
    g_disc_stream.file_type = 0U;
    g_disc_stream.total_sectors = 0U;
    g_disc_stream.completed_sectors = 0U;
    g_disc_stream.file_remaining = 0;
    memset(&g_disc_stream.udf_file_reader, 0,
           sizeof(g_disc_stream.udf_file_reader));
}

static int disc_begin_audio_track(int connection, const struct disc_track_info *track)
{
    if (g_disc_pass_fd < 0 || track == NULL || !track->valid ||
        (track->control & 0x04U) != 0U || track->end_lba <= track->start_lba)
        return disc_http_error(connection, "503 Service Unavailable", "CD audio no disponible\n");
    const uint64_t pcm_size = (uint64_t)(track->end_lba - track->start_lba) *
                              DISC_AUDIO_BYTES_PER_SECTOR;
    if (pcm_size > UINT32_MAX - 36U)
        return disc_http_error(connection, "416 Range Not Satisfiable", "Pista demasiado grande\n");
    unsigned char wav[44] = {
        'R','I','F','F', 0,0,0,0, 'W','A','V','E', 'f','m','t',' ',
        16,0,0,0, 1,0, 2,0, 0x44,0xac,0,0, 0x10,0xb1,2,0,
        4,0, 16,0, 'd','a','t','a', 0,0,0,0
    };
    const uint32_t data_size = (uint32_t)pcm_size;
    wav[4] = (unsigned char)(data_size + 36U);
    wav[5] = (unsigned char)((data_size + 36U) >> 8);
    wav[6] = (unsigned char)((data_size + 36U) >> 16);
    wav[7] = (unsigned char)((data_size + 36U) >> 24);
    wav[40] = (unsigned char)data_size;
    wav[41] = (unsigned char)(data_size >> 8);
    wav[42] = (unsigned char)(data_size >> 16);
    wav[43] = (unsigned char)(data_size >> 24);
    char header[384];
    const int header_size = snprintf(header, sizeof(header),
                                     "HTTP/1.1 200 OK\r\nContent-Type: audio/wav\r\n"
                                     "Content-Length: %llu\r\nConnection: close\r\n"
                                     "Cache-Control: no-store\r\n\r\n",
                                     (unsigned long long)(pcm_size + sizeof(wav)));
    if (header_size <= 0 || (size_t)header_size >= sizeof(header) ||
        send_all(connection, header, (size_t)header_size) != 0 ||
        send_all(connection, wav, sizeof(wav)) != 0)
        return -1;
    g_disc_stream.client = connection;
    g_disc_stream.kind = DISC_STREAM_AUDIO;
    g_disc_stream.track = track;
    g_disc_stream.total_sectors = track->end_lba - track->start_lba;
    g_disc_stream.completed_sectors = 0U;
    report("CDDA playback begin track=%u sectors=%u", track->number,
           g_disc_stream.total_sectors);
    return 1;
}

static const char *disc_file_content_type(unsigned char type)
{
    switch (type)
    {
    case RADIO_DISC_ENTRY_MP3: return "audio/mpeg";
    case RADIO_DISC_ENTRY_AAC: return "audio/aac";
    case RADIO_DISC_ENTRY_FLAC: return "audio/flac";
    case RADIO_DISC_ENTRY_WAV: return "audio/wav";
    default: return "application/octet-stream";
    }
}

static int disc_begin_audio_file(int connection, unsigned id)
{
    if (id >= g_disc_audio_file_count)
        return disc_http_error(connection, "404 Not Found", "Archivo de audio no encontrado\n");
    const struct disc_audio_source *source = &g_disc_audio_sources[id];
    int fd = -1;
    uint64_t file_size = 0U;
    if (source->source_kind == DISC_AUDIO_SOURCE_MOUNTED)
    {
        fd = open(source->location.mounted_path, O_RDONLY);
        if (fd < 0)
            return disc_http_error(connection, "404 Not Found", "No se pudo leer el archivo\n");
        struct stat info;
        if (fstat(fd, &info) != 0 || !S_ISREG(info.st_mode) || info.st_size < 0)
        {
            close(fd);
            return disc_http_error(connection, "404 Not Found", "Archivo de audio no válido\n");
        }
        file_size = (uint64_t)info.st_size;
    }
    else if (source->source_kind == DISC_AUDIO_SOURCE_UDF_RAW)
    {
        file_size = source->location.udf_file.size;
        if (radio_udf_file_reader_open(&g_disc_udf_reader,
                &source->location.udf_file, &g_disc_stream.udf_file_reader) != 0)
            return disc_http_error(connection, "415 Unsupported Media Type",
                                   "Asignación UDF no compatible\n");
    }
    else
        return disc_http_error(connection, "404 Not Found", "Origen de audio no disponible\n");
    if (file_size == 0U)
    {
        if (fd >= 0)
            close(fd);
        return disc_http_error(connection, "404 Not Found", "Archivo de audio vacío\n");
    }
    char header[512];
    const int header_size = snprintf(header, sizeof(header),
                                     "HTTP/1.1 200 OK\r\nContent-Type: %s\r\n"
                                     "Content-Length: %llu\r\nConnection: close\r\n"
                                     "Cache-Control: no-store\r\n\r\n",
                                     disc_file_content_type(source->type),
                                     (unsigned long long)file_size);
    if (header_size <= 0 || (size_t)header_size >= sizeof(header) ||
        send_all(connection, header, (size_t)header_size) != 0)
    {
        if (fd >= 0)
            close(fd);
        memset(&g_disc_stream.udf_file_reader, 0,
               sizeof(g_disc_stream.udf_file_reader));
        return -1;
    }
    g_disc_stream.client = connection;
    g_disc_stream.file_fd = fd;
    g_disc_stream.kind = DISC_STREAM_FILE;
    g_disc_stream.file_type = source->type;
    g_disc_stream.file_remaining = file_size;
    report("CD data-audio playback begin entry=%u source=%s type=%u bytes=%llu",
           id, source->source_kind == DISC_AUDIO_SOURCE_UDF_RAW ? "udf-raw" : "mounted",
           (unsigned)source->type, (unsigned long long)file_size);
    return 1;
}

/* Send at most one SCSI audio batch or one MP3 block per main-loop pass so
 * the payload can answer app keepalives and stop requests while music plays. */
static int disc_stream_step(void)
{
    if (g_disc_stream.kind == DISC_STREAM_NONE || g_disc_stream.client < 0)
        return 0;
    if (g_disc_stream.kind == DISC_STREAM_AUDIO)
    {
        const struct disc_track_info *track = g_disc_stream.track;
        if (track == NULL || g_disc_stream.completed_sectors >= g_disc_stream.total_sectors)
        {
            if (track != NULL)
                report("CDDA playback end result=PASS track=%u sectors=%u",
                       track->number, g_disc_stream.total_sectors);
            disc_close_stream();
            return 0;
        }
        uint32_t batch = g_disc_stream.total_sectors - g_disc_stream.completed_sectors;
        if (batch > DISC_READ_BATCH_SECTORS)
            batch = DISC_READ_BATCH_SECTORS;
        const uint32_t lba = track->start_lba + g_disc_stream.completed_sectors;
        unsigned char cdb[12] = {0xbeU, 0x04U, 0,0,0,0,0,0,0,0x10U,0,0};
        cdb[2] = (unsigned char)(lba >> 24);
        cdb[3] = (unsigned char)(lba >> 16);
        cdb[4] = (unsigned char)(lba >> 8);
        cdb[5] = (unsigned char)lba;
        cdb[6] = (unsigned char)(batch >> 16);
        cdb[7] = (unsigned char)(batch >> 8);
        cdb[8] = (unsigned char)batch;
        unsigned char pcm[DISC_READ_BATCH_SECTORS * DISC_AUDIO_BYTES_PER_SECTOR];
        const size_t bytes = (size_t)batch * DISC_AUDIO_BYTES_PER_SECTOR;
        if (disc_issue_read(cdb, sizeof(cdb), pcm, bytes, NULL) != 0)
        {
            report("CDDA playback read failed track=%u lba=%u sectors=%u",
                   track->number, lba, batch);
            disc_close_stream();
            return 0;
        }
        /* MMC READ CD returns CD-DA samples in byte order suitable for
         * little-endian PCM/WAVE. Preserve the bytes for the WAVE consumer. */
        if (send_all(g_disc_stream.client, pcm, bytes) != 0)
        {
            report("CDDA playback client stopped track=%u lba=%u", track->number, lba);
            disc_close_stream();
            return 0;
        }
        g_disc_stream.completed_sectors += batch;
        if (g_disc_stream.completed_sectors == g_disc_stream.total_sectors ||
            g_disc_stream.completed_sectors % 7500U == 0U)
            report("CDDA playback progress track=%u sectors=%u/%u",
                   track->number, g_disc_stream.completed_sectors,
                   g_disc_stream.total_sectors);
        return 1;
    }
    if (g_disc_stream.kind == DISC_STREAM_FILE)
    {
        if (g_disc_stream.file_remaining == 0U)
        {
            disc_close_stream();
            return 0;
        }
        unsigned char buffer[BRIDGE_CHUNK];
        size_t wanted = g_disc_stream.file_remaining < sizeof(buffer)
                            ? (size_t)g_disc_stream.file_remaining : sizeof(buffer);
        size_t amount = 0U;
        if (g_disc_stream.file_fd < 0)
        {
            if (radio_udf_file_reader_next(&g_disc_udf_reader,
                    &g_disc_stream.udf_file_reader, buffer, wanted, &amount) != 0)
            {
                report("CD UDF playback read failed entry_type=%u remaining=%llu",
                       (unsigned)g_disc_stream.file_type,
                       (unsigned long long)g_disc_stream.file_remaining);
                disc_close_stream();
                return 0;
            }
        }
        else
        {
            ssize_t count = read(g_disc_stream.file_fd, buffer, wanted);
            if (count < 0 && errno == EINTR)
                return 1;
            if (count > 0)
                amount = (size_t)count;
        }
        if (amount == 0U || send_all(g_disc_stream.client, buffer, amount) != 0)
        {
            report("CD audio-file stream ended early source=%s errno=%d",
                   g_disc_stream.file_fd < 0 ? "udf-raw" : "mounted", errno);
            disc_close_stream();
            return 0;
        }
        g_disc_stream.file_remaining -= amount;
        if (g_disc_stream.file_remaining == 0U)
        {
            report("CD data-audio playback end result=PASS type=%u",
                   (unsigned)g_disc_stream.file_type);
            disc_close_stream();
            return 0;
        }
        return 1;
    }
    disc_close_stream();
    return 0;
}

static int disc_parse_index(const char *target, unsigned *track_number,
                            unsigned *file_id, int *is_file)
{
    static const char track_prefix[] = "/track/";
    static const char file_prefix[] = "/file/";
    const char *cursor;
    unsigned long value = 0U;
    if (strncmp(target, track_prefix, sizeof(track_prefix) - 1U) == 0)
    {
        cursor = target + sizeof(track_prefix) - 1U;
        *is_file = 0;
    }
    else if (strncmp(target, file_prefix, sizeof(file_prefix) - 1U) == 0)
    {
        cursor = target + sizeof(file_prefix) - 1U;
        *is_file = 1;
    }
    else
        return -1;
    if (*cursor < '0' || *cursor > '9')
        return -1;
    while (*cursor >= '0' && *cursor <= '9')
    {
        value = value * 10U + (unsigned long)(*cursor++ - '0');
        if (value > 65535U)
            return -1;
    }
    if (*cursor != '\0')
        return -1;
    if (*is_file)
        *file_id = (unsigned)value;
    else
        *track_number = (unsigned)value;
    return 0;
}

static void handle_disc_client(int connection)
{
    struct timeval timeout = {3, 0};
    (void)setsockopt(connection, SOL_SOCKET, SO_RCVTIMEO, &timeout, sizeof(timeout));
    (void)setsockopt(connection, SOL_SOCKET, SO_SNDTIMEO, &timeout, sizeof(timeout));
    char request[2049];
    size_t received = 0U;
    size_t header_size = 0U;
    while (received < sizeof(request) - 1U && header_size == 0U)
    {
        ssize_t count = recv(connection, request + received,
                             sizeof(request) - 1U - received, 0);
        if (count < 0 && errno == EINTR)
            continue;
        if (count <= 0)
            break;
        received += (size_t)count;
        request[received] = '\0';
        char *separator = strstr(request, "\r\n\r\n");
        if (separator != NULL)
            header_size = (size_t)(separator - request) + 4U;
    }
    int streaming = 0;
    if (header_size == 0U || strncmp(request, "GET ", 4U) != 0)
        (void)disc_http_error(connection, "400 Bad Request", "Petición inválida\n");
    else
    {
        char *target = request + 4U;
        char *end = strchr(target, ' ');
        if (end == NULL || (size_t)(end - target) >= 256U)
            (void)disc_http_error(connection, "400 Bad Request", "Ruta inválida\n");
        else
        {
            char path[256];
            const size_t path_size = (size_t)(end - target);
            memcpy(path, target, path_size);
            path[path_size] = '\0';
            unsigned track_number = 0U;
            unsigned file_id = 0U;
            int is_file = 0;
            if (disc_parse_index(path, &track_number, &file_id, &is_file) != 0)
                (void)disc_http_error(connection, "404 Not Found", "Ruta CD no encontrada\n");
            else if (is_file)
                streaming = disc_begin_audio_file(connection, file_id);
            else if (track_number >= 100U)
                (void)disc_http_error(connection, "404 Not Found", "Pista CD no encontrada\n");
            else
                streaming = disc_begin_audio_track(connection, &g_disc_tracks[track_number]);
        }
    }
    if (streaming == 1)
    {
        while (g_disc_stream.kind != DISC_STREAM_NONE)
        {
            if (disc_stream_step() == 0)
                break;
        }
    }
    if (g_disc_stream.client >= 0)
        disc_close_stream();
    else if (streaming != 1)
        close(connection);
}

static void *disc_worker_main(void *unused)
{
    (void)unused;
    for (;;)
    {
        int connection = -1;
        int scan = 0;
        pthread_mutex_lock(&g_disc_worker_mutex);
        while (!g_disc_worker_shutdown && !g_disc_scan_requested &&
               g_disc_http_queue_count == 0U)
            pthread_cond_wait(&g_disc_worker_condition, &g_disc_worker_mutex);
        if (g_disc_worker_shutdown)
        {
            int pending[DISC_HTTP_QUEUE_CAPACITY];
            size_t pending_count = 0U;
            while (g_disc_http_queue_count > 0U)
            {
                pending[pending_count++] =
                    g_disc_http_queue[g_disc_http_queue_head];
                g_disc_http_queue_head =
                    (g_disc_http_queue_head + 1U) % DISC_HTTP_QUEUE_CAPACITY;
                --g_disc_http_queue_count;
            }
            pthread_mutex_unlock(&g_disc_worker_mutex);
            for (size_t i = 0U; i < pending_count; ++i)
                close(pending[i]);
            return NULL;
        }
        if (g_disc_scan_requested)
        {
            g_disc_scan_requested = 0;
            g_disc_scan_active = 1;
            scan = 1;
        }
        else
        {
            connection = g_disc_http_queue[g_disc_http_queue_head];
            g_disc_http_queue_head =
                (g_disc_http_queue_head + 1U) % DISC_HTTP_QUEUE_CAPACITY;
            --g_disc_http_queue_count;
        }
        pthread_mutex_unlock(&g_disc_worker_mutex);

        if (scan)
        {
            unsigned char index[RADIO_DISC_INDEX_MAX_BYTES];
            size_t index_size = 0U;
            const int result = disc_refresh_index(index, sizeof(index), &index_size);
            int superseded = 0;
            pthread_mutex_lock(&g_disc_worker_mutex);
            if (g_disc_scan_requested)
            {
                /* A newer scan was requested while this result was in flight. */
                g_disc_index_cache_valid = 0;
                superseded = 1;
            }
            else if (result == 0 && index_size <= sizeof(g_disc_index_cache))
            {
                memcpy(g_disc_index_cache, index, index_size);
                g_disc_index_cache_size = index_size;
                g_disc_index_cache_valid = 1;
            }
            else
            {
                memset(g_disc_index_cache, 0, RADIO_DISC_INDEX_HEADER_BYTES);
                memcpy(g_disc_index_cache, "PRCD", 4U);
                g_disc_index_cache[4] = RADIO_DISC_INDEX_VERSION;
                g_disc_index_cache[5] = RADIO_DISC_MEDIA_ERROR;
                disc_write_u16be(g_disc_index_cache + 6U, 0U);
                g_disc_index_cache[RADIO_DISC_HEADER_DEVICE_AVAILABLE_OFFSET] =
                    (unsigned char)disc_device_available();
                g_disc_index_cache_size = RADIO_DISC_INDEX_HEADER_BYTES;
                g_disc_index_cache_valid = 1;
            }
            g_disc_scan_active = 0;
            pthread_cond_broadcast(&g_disc_worker_condition);
            pthread_mutex_unlock(&g_disc_worker_mutex);
            report("USB optical scan worker finished result=%s bytes=%u",
                   superseded ? "SUPERSEDED" : (result == 0 ? "PASS" : "FAIL"),
                   (unsigned)index_size);
            continue;
        }

        handle_disc_client(connection);
    }
}

static int disc_worker_start(void)
{
    pthread_mutex_lock(&g_disc_worker_mutex);
    g_disc_worker_shutdown = 0;
    g_disc_scan_requested = 0;
    g_disc_scan_active = 0;
    g_disc_index_cache_valid = 0;
    g_disc_index_cache_size = 0U;
    g_disc_http_queue_head = 0U;
    g_disc_http_queue_count = 0U;
    pthread_mutex_unlock(&g_disc_worker_mutex);
    pthread_attr_t attributes;
    int result = pthread_attr_init(&attributes);
    if (result != 0)
    {
        report("CD worker stack attribute init failed error=%d", result);
        return -1;
    }
    result = pthread_attr_setstacksize(&attributes, DISC_WORKER_STACK_BYTES);
    if (result != 0)
    {
        report("CD worker stack sizing failed bytes=%u error=%d",
               (unsigned)DISC_WORKER_STACK_BYTES, result);
        pthread_attr_destroy(&attributes);
        return -1;
    }
    result = pthread_create(&g_disc_worker_thread, &attributes,
                            disc_worker_main, NULL);
    pthread_attr_destroy(&attributes);
    if (result != 0)
    {
        report("CD worker start failed error=%d", result);
        return -1;
    }
    pthread_mutex_lock(&g_disc_worker_mutex);
    g_disc_worker_started = 1;
    pthread_mutex_unlock(&g_disc_worker_mutex);
    report("CD worker started; optical I/O is isolated from bridge loop stack_bytes=%u",
           (unsigned)DISC_WORKER_STACK_BYTES);
    return 0;
}

static void disc_worker_stop(void)
{
    int queued[DISC_HTTP_QUEUE_CAPACITY];
    size_t queued_count = 0U;
    pthread_mutex_lock(&g_disc_worker_mutex);
    if (!g_disc_worker_started)
    {
        pthread_mutex_unlock(&g_disc_worker_mutex);
        return;
    }
    g_disc_worker_shutdown = 1;
    while (g_disc_http_queue_count > 0U)
    {
        queued[queued_count++] = g_disc_http_queue[g_disc_http_queue_head];
        g_disc_http_queue_head =
            (g_disc_http_queue_head + 1U) % DISC_HTTP_QUEUE_CAPACITY;
        --g_disc_http_queue_count;
    }
    pthread_cond_broadcast(&g_disc_worker_condition);
    pthread_mutex_unlock(&g_disc_worker_mutex);
    for (size_t i = 0U; i < queued_count; ++i)
        close(queued[i]);
    const int result = pthread_join(g_disc_worker_thread, NULL);
    if (result != 0)
        report("CD worker join failed error=%d", result);
    pthread_mutex_lock(&g_disc_worker_mutex);
    g_disc_worker_started = 0;
    pthread_mutex_unlock(&g_disc_worker_mutex);
    report("CD worker stopped");
}

static int disc_worker_enqueue_http(int connection)
{
    pthread_mutex_lock(&g_disc_worker_mutex);
    if (!g_disc_worker_started || g_disc_worker_shutdown ||
        g_disc_http_queue_count >= DISC_HTTP_QUEUE_CAPACITY)
    {
        pthread_mutex_unlock(&g_disc_worker_mutex);
        return -1;
    }
    const size_t tail = (g_disc_http_queue_head + g_disc_http_queue_count) %
                        DISC_HTTP_QUEUE_CAPACITY;
    g_disc_http_queue[tail] = connection;
    ++g_disc_http_queue_count;
    pthread_cond_signal(&g_disc_worker_condition);
    pthread_mutex_unlock(&g_disc_worker_mutex);
    return 0;
}

static int serve_disc_index(int connection, int *disc_listener, int force_refresh)
{
    unsigned char index[RADIO_DISC_INDEX_MAX_BYTES];
    size_t index_size = 0U;
    int pending = 0;
    pthread_mutex_lock(&g_disc_worker_mutex);
    if (!g_disc_worker_started || g_disc_worker_shutdown)
    {
        pthread_mutex_unlock(&g_disc_worker_mutex);
        return send_response(connection, BRIDGE_IO_ERROR, 0U);
    }
    if (force_refresh)
    {
        g_disc_index_cache_valid = 0;
        if (!g_disc_scan_active && !g_disc_scan_requested)
        {
            g_disc_scan_requested = 1;
            pthread_cond_signal(&g_disc_worker_condition);
        }
    }
    else if (!g_disc_index_cache_valid && !g_disc_scan_active &&
             !g_disc_scan_requested)
    {
        g_disc_scan_requested = 1;
        pthread_cond_signal(&g_disc_worker_condition);
    }
    if (g_disc_index_cache_valid)
    {
        index_size = g_disc_index_cache_size;
        memcpy(index, g_disc_index_cache, index_size);
    }
    else
        pending = 1;
    pthread_mutex_unlock(&g_disc_worker_mutex);

    if (pending)
    {
        memset(index, 0, RADIO_DISC_INDEX_HEADER_BYTES);
        memcpy(index, "PRCD", 4U);
        index[4] = RADIO_DISC_INDEX_VERSION;
        index[5] = RADIO_DISC_MEDIA_PENDING;
        index[RADIO_DISC_HEADER_DEVICE_AVAILABLE_OFFSET] =
            (unsigned char)disc_device_available();
        index_size = RADIO_DISC_INDEX_HEADER_BYTES;
    }
    if (!pending && index[5] != 0U && index[5] != RADIO_DISC_MEDIA_ERROR)
    {
        if (*disc_listener < 0)
            *disc_listener = open_listener(DISC_PORT, 1, "DISC");
    }
    else if (!pending && *disc_listener >= 0)
    {
        close(*disc_listener);
        *disc_listener = -1;
    }
    if (!pending && *disc_listener < 0 && index[5] != 0U &&
        index[5] != RADIO_DISC_MEDIA_ERROR)
        return send_response(connection, BRIDGE_IO_ERROR, 0U);
    if (send_response(connection, BRIDGE_OK, (uint32_t)index_size) != 0)
        return -1;
    return send_all(connection, index, index_size);
}

static int serve_bridge_request(int connection, int *stop_requested, int *aux_listener,
                                int *disc_listener)
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
        return serve_get(connection, file_id,
                          file_id == 6U ? AUX_MAX_STORAGE : BRIDGE_MAX_FILE);
    if (operation == BRIDGE_PUT)
        return serve_put(connection, file_id, size);
    if (operation == BRIDGE_GET_AUX_BUFFER && file_id == 0 && size == 0)
        return serve_get(connection, 6U, AUX_MAX_STORAGE);
    if (operation == BRIDGE_PUT_AUX_BUFFER && file_id == 0 &&
        size >= AUX_ENVELOPE_HEADER_BYTES && size <= AUX_MAX_STORAGE)
        return serve_put(connection, 6U, size);
    if (operation == BRIDGE_GET_DISC_INDEX && file_id <= 1U && size == 0U)
        return serve_disc_index(connection, disc_listener, file_id == 1U);
    if (operation == BRIDGE_GET_USB_INDEX && file_id <= 1U && size == 0U)
        return serve_usb_index(connection, file_id == 1U);
    if (operation == BRIDGE_STATUS && file_id == 0 && size == 0)
    {
        const unsigned char flags = g_aux_ready ? 1U : 0U;
        if (send_response(connection, BRIDGE_OK, 1) != 0)
            return -1;
        return send_all(connection, &flags, sizeof(flags));
    }
    if (operation == BRIDGE_START_AUX && file_id == 0 && size == 0)
    {
        if (!g_aux_worker_started && aux_worker_start() != 0)
        {
            g_aux_ready = 0;
            g_aux_start_failed = 1;
            report("AUX start request failed; isolated HTTP worker unavailable");
            return send_response(connection, BRIDGE_IO_ERROR, 0);
        }
        if (*aux_listener < 0)
        {
            *aux_listener = open_listener(AUX_PORT, 0, "AUX");
            g_aux_ready = *aux_listener >= 0;
        }
        if (!g_aux_ready)
        {
            g_aux_start_failed = 1;
            report("AUX start request failed; listener unavailable");
            return send_response(connection, BRIDGE_IO_ERROR, 0);
        }
        g_aux_start_failed = 0;
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
        g_aux_start_failed = 0;
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

static int usb_supported_filesystem(const char *name)
{
    return name != NULL &&
           (strcasecmp(name, "msdosfs") == 0 || strcasecmp(name, "fat") == 0 ||
            strcasecmp(name, "vfat") == 0 || strcasecmp(name, "exfat") == 0 ||
            strcasecmp(name, "exfatfs") == 0 || strcasecmp(name, "ntfs") == 0 ||
            strcasecmp(name, "ntfs-3g") == 0);
}

static int usb_device_name_is_partition(const char *name)
{
    if (name == NULL || strncmp(name, "da", 2U) != 0)
        return 0;
    const char *cursor = name + 2U;
    if (*cursor < '0' || *cursor > '9')
        return 0;
    while (*cursor >= '0' && *cursor <= '9')
        ++cursor;
    if (*cursor != 'p' && *cursor != 's')
        return 0;
    ++cursor;
    if (*cursor < '0' || *cursor > '9')
        return 0;
    while (*cursor >= '0' && *cursor <= '9')
        ++cursor;
    return *cursor == '\0';
}

static int usb_source_is_usb_partition(const char *source)
{
    if (source == NULL)
        return 0;
    const char *device = strncmp(source, "/dev/", 5U) == 0 ? source + 5U : source;
    return usb_device_name_is_partition(device);
}

static int usb_same_device_source(const char *left, const char *right)
{
    if (left == NULL || right == NULL)
        return 0;
    if (strncmp(left, "/dev/", 5U) == 0)
        left += 5U;
    if (strncmp(right, "/dev/", 5U) == 0)
        right += 5U;
    return strcmp(left, right) == 0;
}

static int usb_source_is_mounted(const char *source)
{
    if (source == NULL)
        return 0;
    struct statfs *mounts = NULL;
    const int count = getmntinfo(&mounts, MNT_NOWAIT);
    for (int index = 0; index < count; ++index)
    {
        if (usb_same_device_source(mounts[index].f_mntfromname, source))
            return 1;
    }
    return 0;
}

static int usb_prepare_mount_root(unsigned number, char *root, size_t capacity)
{
    struct statfs base;
    if (statfs("/mnt", &base) != 0 || strcmp(base.f_mntonname, "/mnt") != 0 ||
        strcmp(base.f_fstypename, "tmpfs") != 0)
        return -1;
    const int length = snprintf(root, capacity, "/mnt/usb%u", number);
    if (length <= 0 || (size_t)length >= capacity)
        return -1;
    struct stat root_info;
    if (lstat(root, &root_info) != 0)
    {
        if (errno != ENOENT || mkdir(root, 0755) != 0)
            return -1;
        if (lstat(root, &root_info) != 0)
            return -1;
    }
    if (!S_ISDIR(root_info.st_mode))
        return -1;
    struct statfs current;
    if (statfs(root, &current) != 0 || strcmp(current.f_mntonname, "/mnt") != 0 ||
        strcmp(current.f_fstypename, "tmpfs") != 0)
        return -1;
    DIR *directory = opendir(root);
    if (directory == NULL)
        return -1;
    int empty = 1;
    struct dirent *entry;
    while ((entry = readdir(directory)) != NULL)
    {
        if (strcmp(entry->d_name, ".") != 0 && strcmp(entry->d_name, "..") != 0)
        {
            empty = 0;
            break;
        }
    }
    closedir(directory);
    return empty ? 0 : -1;
}

static int usb_mount_partition_readonly(const char *source, const char *root,
                                       int *first_error, const char **first_error_type)
{
    static const char *const filesystem_types[] = {
        "msdosfs", "exfat", "exfatfs", "ntfs", "ntfs-3g"
    };
    for (size_t attempt = 0U;
         attempt < sizeof(filesystem_types) / sizeof(filesystem_types[0]); ++attempt)
    {
        struct iovec options[6];
        const char *names[3] = {"fstype", "fspath", "from"};
        const char *values[3] = {filesystem_types[attempt], root, source};
        for (size_t index = 0U; index < 3U; ++index)
        {
            options[index * 2U].iov_base = (void *)names[index];
            options[index * 2U].iov_len = strlen(names[index]) + 1U;
            options[index * 2U + 1U].iov_base = (void *)values[index];
            options[index * 2U + 1U].iov_len = strlen(values[index]) + 1U;
        }
        if (nmount(options, 6U, MNT_RDONLY) != 0)
        {
            if (first_error != NULL && *first_error == 0)
            {
                *first_error = errno;
                if (first_error_type != NULL)
                    *first_error_type = filesystem_types[attempt];
            }
            continue;
        }
        struct statfs mounted;
        if (statfs(root, &mounted) == 0 &&
            strcmp(mounted.f_mntonname, root) == 0 &&
            usb_same_device_source(mounted.f_mntfromname, source) &&
            (mounted.f_flags & MNT_RDONLY) != 0 &&
            usb_supported_filesystem(mounted.f_fstypename))
        {
            if (g_usb_owned_mount_count < USB_MAX_MOUNT_ROOTS)
            {
                const unsigned slot = g_usb_owned_mount_count++;
                (void)snprintf(g_usb_owned_mounts[slot],
                               sizeof(g_usb_owned_mounts[slot]), "%s", root);
                (void)snprintf(g_usb_owned_sources[slot],
                               sizeof(g_usb_owned_sources[slot]), "%s", source);
            }
            return 1;
        }
        report("USB read-only mount verification failed root=%s; left untouched", root);
        return -1;
    }
    return 0;
}

static unsigned usb_mount_discovered_partitions(void)
{
    DIR *devices = opendir("/dev");
    if (devices == NULL)
        return 0U;
    unsigned mounted_count = 0U;
    unsigned candidate_count = 0U;
    unsigned root_number = 0U;
    int first_mount_error = 0;
    const char *first_mount_error_type = NULL;
    char first_mount_error_source[RADIO_USB_PATH_BYTES] = {0};
    struct dirent *entry;
    while (root_number < USB_MAX_MOUNT_ROOTS && candidate_count < USB_MAX_DEVICE_CANDIDATES &&
           (entry = readdir(devices)) != NULL)
    {
        if (!usb_device_name_is_partition(entry->d_name))
            continue;
        ++candidate_count;
        char source[RADIO_USB_PATH_BYTES];
        const int source_length = snprintf(source, sizeof(source), "/dev/%s", entry->d_name);
        if (source_length <= 0 || (size_t)source_length >= sizeof(source) ||
            usb_source_is_mounted(source))
            continue;
        struct stat device_info;
        if (lstat(source, &device_info) != 0 ||
            (!S_ISCHR(device_info.st_mode) && !S_ISBLK(device_info.st_mode)))
            continue;
        char root[RADIO_USB_PATH_BYTES];
        if (usb_prepare_mount_root(root_number, root, sizeof(root)) != 0)
        {
            ++root_number;
            continue;
        }
        int mount_error = 0;
        const char *mount_error_type = NULL;
        const int result = usb_mount_partition_readonly(source, root,
                                                       &mount_error, &mount_error_type);
        if (result > 0)
        {
            ++mounted_count;
            ++root_number;
        }
        else if (result < 0)
            ++root_number;
        else if (mount_error != 0 && first_mount_error == 0)
        {
            first_mount_error = mount_error;
            first_mount_error_type = mount_error_type;
            (void)snprintf(first_mount_error_source, sizeof(first_mount_error_source),
                           "%s", source);
        }
    }
    closedir(devices);
    if (candidate_count != 0U)
        report("USB read-only mount discovery candidates=%u mounted=%u first_failure=%s type=%s errno=%d",
               candidate_count, mounted_count,
               first_mount_error_source[0] != '\0' ? first_mount_error_source : "none",
               first_mount_error_type != NULL ? first_mount_error_type : "none",
               first_mount_error);
    return mounted_count;
}

static void usb_unmount_owned_partitions(void)
{
    char retained_mounts[USB_MAX_MOUNT_ROOTS][RADIO_USB_PATH_BYTES] = {{0}};
    char retained_sources[USB_MAX_MOUNT_ROOTS][RADIO_USB_PATH_BYTES] = {{0}};
    unsigned retained_count = 0U;
    for (unsigned slot = 0U; slot < g_usb_owned_mount_count; ++slot)
    {
        struct statfs mounted;
        if (statfs(g_usb_owned_mounts[slot], &mounted) != 0)
        {
            report("USB unmount deferred: mount identity unavailable root=%s errno=%d",
                   g_usb_owned_mounts[slot], errno);
            memcpy(retained_mounts[retained_count], g_usb_owned_mounts[slot],
                   sizeof(retained_mounts[retained_count]));
            memcpy(retained_sources[retained_count], g_usb_owned_sources[slot],
                   sizeof(retained_sources[retained_count]));
            ++retained_count;
            continue;
        }
        if (strcmp(mounted.f_mntonname, g_usb_owned_mounts[slot]) != 0 ||
            !usb_same_device_source(mounted.f_mntfromname, g_usb_owned_sources[slot]) ||
            (mounted.f_flags & MNT_RDONLY) == 0 ||
            !usb_supported_filesystem(mounted.f_fstypename))
        {
            report("USB unmount skipped: mount identity changed root=%s",
                   g_usb_owned_mounts[slot]);
            continue;
        }
        if (unmount(g_usb_owned_mounts[slot], 0) != 0)
        {
            report("USB read-only unmount failed root=%s errno=%d",
                   g_usb_owned_mounts[slot], errno);
            memcpy(retained_mounts[retained_count], g_usb_owned_mounts[slot],
                   sizeof(retained_mounts[retained_count]));
            memcpy(retained_sources[retained_count], g_usb_owned_sources[slot],
                   sizeof(retained_sources[retained_count]));
            ++retained_count;
        }
        else
            report("USB read-only mount released root=%s", g_usb_owned_mounts[slot]);
    }
    memcpy(g_usb_owned_mounts, retained_mounts, sizeof(g_usb_owned_mounts));
    memcpy(g_usb_owned_sources, retained_sources, sizeof(g_usb_owned_sources));
    g_usb_owned_mount_count = retained_count;
    if (retained_count != 0U)
        report("USB read-only mounts remain owned but mounted count=%u", retained_count);
}

static unsigned char usb_audio_type(const char *name)
{
    const char *dot = strrchr(name, '.');
    if (dot == NULL)
        return 0U;
    if (strcasecmp(dot, ".mp3") == 0)
        return RADIO_USB_ENTRY_MP3;
    if (strcasecmp(dot, ".aac") == 0 || strcasecmp(dot, ".adts") == 0)
        return RADIO_USB_ENTRY_AAC;
    if (strcasecmp(dot, ".flac") == 0)
        return RADIO_USB_ENTRY_FLAC;
    if (strcasecmp(dot, ".wav") == 0)
        return RADIO_USB_ENTRY_WAV;
    return 0U;
}

static void usb_display_name(const char *source, char *output, size_t capacity)
{
    size_t used = 0U;
    while (*source != '\0' && used + 1U < capacity)
    {
        const unsigned char byte = (unsigned char)*source++;
        output[used++] = byte >= 0x20U && byte < 0x7fU ? (char)byte : '?';
    }
    output[used] = '\0';
    if (used == 0U)
        (void)snprintf(output, capacity, "USB audio");
}

static int usb_append_index_entry(unsigned char *index, size_t capacity, size_t *used,
                                  unsigned char type, unsigned id, const char *name)
{
    if (g_usb_scan_file_count >= RADIO_USB_MAX_ENTRIES || name == NULL)
        return -1;
    const size_t name_size = strlen(name);
    if (name_size == 0U || name_size > RADIO_USB_NAME_BYTES || *used > capacity ||
        RADIO_USB_INDEX_RECORD_FIXED_BYTES + name_size > capacity - *used)
        return -1;
    unsigned char *record = index + *used;
    memset(record, 0, RADIO_USB_INDEX_RECORD_FIXED_BYTES);
    record[0] = type;
    disc_write_u16be(record + 1U, id);
    disc_write_u16be(record + 12U, (unsigned)name_size);
    memcpy(record + RADIO_USB_INDEX_RECORD_FIXED_BYTES, name, name_size);
    *used += RADIO_USB_INDEX_RECORD_FIXED_BYTES + name_size;
    return 0;
}

static void usb_scan_directory(const char *path, unsigned depth,
                               unsigned char *index, size_t capacity, size_t *used)
{
    if (depth > DISC_MAX_SCAN_DEPTH || g_usb_scan_nodes >= 8192U ||
        g_usb_scan_file_count >= RADIO_USB_MAX_ENTRIES)
        return;
    DIR *directory = opendir(path);
    if (directory == NULL)
        return;
    struct dirent *entry;
    while (g_usb_scan_nodes < 8192U && g_usb_scan_file_count < RADIO_USB_MAX_ENTRIES &&
           (entry = readdir(directory)) != NULL)
    {
        if (strcmp(entry->d_name, ".") == 0 || strcmp(entry->d_name, "..") == 0)
            continue;
        ++g_usb_scan_nodes;
        char child[RADIO_USB_PATH_BYTES];
        const int length = snprintf(child, sizeof(child), "%s/%.*s", path,
                                    (int)RADIO_USB_NAME_BYTES, entry->d_name);
        if (length <= 0 || (size_t)length >= sizeof(child))
            continue;
        struct stat info;
        if (lstat(child, &info) != 0)
            continue;
        if (S_ISDIR(info.st_mode))
            usb_scan_directory(child, depth + 1U, index, capacity, used);
        else if (S_ISREG(info.st_mode))
        {
            const unsigned char type = usb_audio_type(entry->d_name);
            if (type == 0U)
                continue;
            const unsigned id = g_usb_scan_file_count;
            char display[RADIO_USB_NAME_BYTES + 1U];
            usb_display_name(entry->d_name, display, sizeof(display));
            if (usb_append_index_entry(index, capacity, used, type, id, display) != 0)
                break;
            memcpy(g_usb_scan_paths[id], child, (size_t)length + 1U);
            g_usb_scan_types[id] = type;
            ++g_usb_scan_file_count;
        }
    }
    closedir(directory);
}

static int usb_refresh_index(unsigned char *index, size_t capacity, size_t *index_size)
{
    if (index == NULL || index_size == NULL || capacity < RADIO_USB_INDEX_HEADER_BYTES)
        return -1;
    memset(index, 0, RADIO_USB_INDEX_HEADER_BYTES);
    memcpy(index, "PRUS", 4U);
    index[4] = RADIO_USB_INDEX_VERSION;
    g_usb_scan_file_count = 0U;
    g_usb_scan_nodes = 0U;
    g_usb_scan_device_available = 0;
    memset(g_usb_scan_paths, 0, sizeof(g_usb_scan_paths));
    memset(g_usb_scan_types, 0, sizeof(g_usb_scan_types));
    size_t used = RADIO_USB_INDEX_HEADER_BYTES;
    (void)usb_mount_discovered_partitions();
    struct statfs *mounts = NULL;
    const int mount_count = getmntinfo(&mounts, MNT_NOWAIT);
    for (int mount_index = 0; mount_index < mount_count; ++mount_index)
    {
        const struct statfs *mounted = &mounts[mount_index];
        const size_t root_length = strlen(mounted->f_mntonname);
        if (root_length == 0U || root_length >= RADIO_USB_PATH_BYTES ||
            strncmp(mounted->f_mntonname, "/mnt/", 5U) != 0 ||
            !usb_source_is_usb_partition(mounted->f_mntfromname) ||
            !usb_supported_filesystem(mounted->f_fstypename))
            continue;
        char root[RADIO_USB_PATH_BYTES];
        memcpy(root, mounted->f_mntonname, root_length + 1U);
        g_usb_scan_device_available = 1;
        usb_scan_directory(root, 0U, index, capacity, &used);
    }
    index[5] = g_usb_scan_file_count != 0U ? 1U : (g_usb_scan_device_available ? 2U : 0U);
    disc_write_u16be(index + 6U, g_usb_scan_file_count);
    index[RADIO_USB_HEADER_DEVICE_AVAILABLE_OFFSET] =
        (unsigned char)g_usb_scan_device_available;
    *index_size = used;
    report("USB audio scan result=PASS mounted=%u files=%u nodes=%u roots=mounted-usb-partitions access=read-only",
           (unsigned)g_usb_scan_device_available, g_usb_scan_file_count, g_usb_scan_nodes);
    return 0;
}

static int usb_parse_file_target(const char *target, unsigned *file_id)
{
    static const char prefix[] = "/usb/";
    if (target == NULL || file_id == NULL || strncmp(target, prefix, sizeof(prefix) - 1U) != 0)
        return -1;
    const char *cursor = target + sizeof(prefix) - 1U;
    if (*cursor == '\0')
        return -1;
    unsigned value = 0U;
    while (*cursor >= '0' && *cursor <= '9')
    {
        value = value * 10U + (unsigned)(*cursor++ - '0');
        if (value >= RADIO_USB_MAX_ENTRIES)
            return -1;
    }
    if (*cursor != '\0')
        return -1;
    *file_id = value;
    return 0;
}

static const char *usb_content_type(unsigned char type)
{
    switch (type)
    {
    case RADIO_USB_ENTRY_MP3: return "audio/mpeg";
    case RADIO_USB_ENTRY_AAC: return "audio/aac";
    case RADIO_USB_ENTRY_FLAC: return "audio/flac";
    case RADIO_USB_ENTRY_WAV: return "audio/wav";
    default: return "application/octet-stream";
    }
}

static void handle_usb_client(int connection)
{
    struct timeval timeout = {3, 0};
    (void)setsockopt(connection, SOL_SOCKET, SO_RCVTIMEO, &timeout, sizeof(timeout));
    (void)setsockopt(connection, SOL_SOCKET, SO_SNDTIMEO, &timeout, sizeof(timeout));
    char request[2049];
    size_t received = 0U;
    size_t header_size = 0U;
    while (received < sizeof(request) - 1U && header_size == 0U)
    {
        ssize_t count = recv(connection, request + received,
                             sizeof(request) - 1U - received, 0);
        if (count < 0 && errno == EINTR)
            continue;
        if (count <= 0)
            break;
        received += (size_t)count;
        request[received] = '\0';
        char *separator = strstr(request, "\r\n\r\n");
        if (separator != NULL)
            header_size = (size_t)(separator - request) + 4U;
    }
    unsigned id = 0U;
    if (header_size == 0U || strncmp(request, "GET ", 4U) != 0)
        (void)disc_http_error(connection, "400 Bad Request", "Petición inválida\n");
    else
    {
        char *target = request + 4U;
        char *end = strchr(target, ' ');
        if (end == NULL || (size_t)(end - target) >= 256U)
            (void)disc_http_error(connection, "400 Bad Request", "Ruta inválida\n");
        else
        {
            char path[256];
            const size_t path_size = (size_t)(end - target);
            memcpy(path, target, path_size);
            path[path_size] = '\0';
            char source[RADIO_USB_PATH_BYTES];
            unsigned char type = 0U;
            pthread_mutex_lock(&g_usb_mutex);
            const int found = usb_parse_file_target(path, &id) == 0 && id < g_usb_file_count;
            if (found)
            {
                memcpy(source, g_usb_paths[id], sizeof(source));
                type = g_usb_types[id];
            }
            pthread_mutex_unlock(&g_usb_mutex);
            if (!found)
                (void)disc_http_error(connection, "404 Not Found", "Archivo USB no encontrado\n");
            else
            {
                int fd = open(source, O_RDONLY);
                struct stat info;
                if (fd < 0 || fstat(fd, &info) != 0 || !S_ISREG(info.st_mode) || info.st_size <= 0)
                {
                    if (fd >= 0)
                        close(fd);
                    (void)disc_http_error(connection, "404 Not Found", "Archivo USB no legible\n");
                }
                else
                {
                    char header[384];
                    const int header_length = snprintf(
                        header, sizeof(header),
                        "HTTP/1.1 200 OK\r\nContent-Type: %s\r\nContent-Length: %llu\r\n"
                        "Connection: close\r\nCache-Control: no-store\r\n\r\n",
                        usb_content_type(type),
                        (unsigned long long)info.st_size);
                    int ok = header_length > 0 && (size_t)header_length < sizeof(header) &&
                             send_all(connection, header, (size_t)header_length) == 0;
                    unsigned char buffer[BRIDGE_CHUNK];
                    off_t remaining = info.st_size;
                    while (ok && remaining > 0)
                    {
                        size_t wanted = remaining < (off_t)sizeof(buffer)
                                            ? (size_t)remaining : sizeof(buffer);
                        ssize_t amount = read(fd, buffer, wanted);
                        if (amount < 0 && errno == EINTR)
                            continue;
                        if (amount <= 0 || send_all(connection, buffer, (size_t)amount) != 0)
                        {
                            ok = 0;
                            break;
                        }
                        remaining -= amount;
                    }
                    close(fd);
                    report("USB audio stream id=%u type=%u bytes=%llu result=%s",
                           id, (unsigned)type,
                           (unsigned long long)info.st_size, ok ? "PASS" : "STOPPED");
                }
            }
        }
    }
    close(connection);
}

static void *usb_worker_main(void *unused)
{
    (void)unused;
    int listener = open_listener(RADIO_USB_HTTP_PORT, 1, "USB audio");
    time_t next_listener_retry = listener < 0 ? time(NULL) + 5 : 0;
    for (;;)
    {
        int scan = 0;
        pthread_mutex_lock(&g_usb_mutex);
        if (g_usb_shutdown)
        {
            pthread_mutex_unlock(&g_usb_mutex);
            break;
        }
        if (g_usb_scan_requested)
        {
            g_usb_scan_requested = 0;
            g_usb_scan_active = 1;
            scan = 1;
        }
        pthread_mutex_unlock(&g_usb_mutex);
        if (scan)
        {
            unsigned char index[RADIO_USB_INDEX_MAX_BYTES];
            size_t index_size = 0U;
            const int result = usb_refresh_index(index, sizeof(index), &index_size);
            pthread_mutex_lock(&g_usb_mutex);
            if (g_usb_scan_requested)
                g_usb_cache_valid = 0;
            else if (result == 0 && index_size <= sizeof(g_usb_index_cache))
            {
                memcpy(g_usb_paths, g_usb_scan_paths, sizeof(g_usb_paths));
                memcpy(g_usb_types, g_usb_scan_types, sizeof(g_usb_types));
                g_usb_file_count = g_usb_scan_file_count;
                g_usb_device_available = g_usb_scan_device_available;
                memcpy(g_usb_index_cache, index, index_size);
                g_usb_index_cache_size = index_size;
                g_usb_cache_valid = 1;
            }
            else
            {
                memset(g_usb_index_cache, 0, RADIO_USB_INDEX_HEADER_BYTES);
                memcpy(g_usb_index_cache, "PRUS", 4U);
                g_usb_index_cache[4] = RADIO_USB_INDEX_VERSION;
                g_usb_index_cache[5] = RADIO_USB_MEDIA_ERROR;
                g_usb_index_cache[RADIO_USB_HEADER_DEVICE_AVAILABLE_OFFSET] =
                    (unsigned char)g_usb_device_available;
                g_usb_index_cache_size = RADIO_USB_INDEX_HEADER_BYTES;
                g_usb_cache_valid = 1;
                memset(g_usb_paths, 0, sizeof(g_usb_paths));
                memset(g_usb_types, 0, sizeof(g_usb_types));
                g_usb_file_count = 0U;
            }
            g_usb_scan_active = 0;
            pthread_mutex_unlock(&g_usb_mutex);
            continue;
        }
        if (listener < 0)
        {
            if (time(NULL) >= next_listener_retry)
            {
                listener = open_listener(RADIO_USB_HTTP_PORT, 1, "USB audio");
                next_listener_retry = time(NULL) + 5;
            }
            usleep(200000U);
            continue;
        }
        fd_set readable;
        FD_ZERO(&readable);
        FD_SET(listener, &readable);
        struct timeval timeout = {0, 200000};
        const int selected = select(listener + 1, &readable, NULL, NULL, &timeout);
        if (selected > 0 && FD_ISSET(listener, &readable))
        {
            int client = accept(listener, NULL, NULL);
            if (client >= 0)
                handle_usb_client(client);
            else if (errno != EINTR)
                report("USB audio accept failed errno=%d", errno);
        }
        else if (selected < 0 && errno != EINTR)
            report("USB audio select failed errno=%d", errno);
    }
    if (listener >= 0)
        close(listener);
    return NULL;
}

static int usb_worker_start(void)
{
    pthread_mutex_lock(&g_usb_mutex);
    g_usb_shutdown = 0;
    g_usb_scan_requested = 0;
    g_usb_scan_active = 0;
    g_usb_cache_valid = 0;
    g_usb_index_cache_size = 0U;
    pthread_mutex_unlock(&g_usb_mutex);
    const int result = pthread_create(&g_usb_thread, NULL, usb_worker_main, NULL);
    if (result != 0)
    {
        report("USB audio worker start failed error=%d", result);
        return -1;
    }
    pthread_mutex_lock(&g_usb_mutex);
    g_usb_thread_started = 1;
    pthread_mutex_unlock(&g_usb_mutex);
    report("USB audio worker started independently from optical I/O");
    return 0;
}

static void usb_worker_stop(void)
{
    pthread_mutex_lock(&g_usb_mutex);
    if (!g_usb_thread_started)
    {
        pthread_mutex_unlock(&g_usb_mutex);
        return;
    }
    g_usb_shutdown = 1;
    pthread_mutex_unlock(&g_usb_mutex);
    const int result = pthread_join(g_usb_thread, NULL);
    if (result != 0)
    {
        report("USB audio worker join failed error=%d", result);
        report("USB read-only mounts left active because worker shutdown was not confirmed");
    }
    else
        usb_unmount_owned_partitions();
    pthread_mutex_lock(&g_usb_mutex);
    g_usb_thread_started = 0;
    pthread_mutex_unlock(&g_usb_mutex);
    report("USB audio worker stopped");
}

static int serve_usb_index(int connection, int force_refresh)
{
    unsigned char index[RADIO_USB_INDEX_MAX_BYTES];
    size_t index_size = 0U;
    int pending = 0;
    unsigned char device_available = 0U;
    pthread_mutex_lock(&g_usb_mutex);
    if (!g_usb_thread_started || g_usb_shutdown)
    {
        pthread_mutex_unlock(&g_usb_mutex);
        return send_response(connection, BRIDGE_IO_ERROR, 0U);
    }
    if (force_refresh)
    {
        g_usb_cache_valid = 0;
        if (!g_usb_scan_requested)
        {
            g_usb_scan_requested = 1;
        }
    }
    else if (!g_usb_cache_valid && !g_usb_scan_active && !g_usb_scan_requested)
    {
        g_usb_scan_requested = 1;
    }
    if (g_usb_cache_valid)
    {
        index_size = g_usb_index_cache_size;
        memcpy(index, g_usb_index_cache, index_size);
    }
    else
        pending = 1;
    device_available = (unsigned char)g_usb_device_available;
    pthread_mutex_unlock(&g_usb_mutex);
    if (pending)
    {
        memset(index, 0, RADIO_USB_INDEX_HEADER_BYTES);
        memcpy(index, "PRUS", 4U);
        index[4] = RADIO_USB_INDEX_VERSION;
        index[5] = RADIO_USB_MEDIA_PENDING;
        index[RADIO_USB_HEADER_DEVICE_AVAILABLE_OFFSET] = device_available;
        index_size = RADIO_USB_INDEX_HEADER_BYTES;
    }
    if (send_response(connection, BRIDGE_OK, (uint32_t)index_size) != 0)
        return -1;
    return send_all(connection, index, index_size);
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
    int found_length = 0;
    while (line < limit && line[0] != '\r' && line[1] != '\n')
    {
        const char *end = strstr(line, "\r\n");
        if (end == NULL || end > limit)
            return -1;
        const char *colon = memchr(line, ':', (size_t)(end - line));
        if (colon != NULL && header_name_is(line, (size_t)(colon - line), "Content-Length"))
        {
            if (found_length)
                return -1;
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
            found_length = 1;
        }
        else if (colon != NULL &&
                 header_name_is(line, (size_t)(colon - line), "Transfer-Encoding"))
            return -1;
        line = end + 2;
    }
    return found_length ? 0 : 1;
}

static int parse_aux_format(const char *headers, size_t header_size, unsigned char *format)
{
    if (headers == NULL || format == NULL || header_size < 4U)
        return -1;
    *format = AUX_FORMAT_AUTO;
    const char *line = strstr(headers, "\r\n");
    if (line == NULL)
        return -1;
    line += 2;
    const char *limit = headers + header_size;
    int found = 0;
    while (line < limit && line[0] != '\r' && line[1] != '\n')
    {
        const char *end = strstr(line, "\r\n");
        if (end == NULL || end > limit)
            return -1;
        const char *colon = memchr(line, ':', (size_t)(end - line));
        if (colon != NULL && header_name_is(line, (size_t)(colon - line), "X-Playlist-Format"))
        {
            if (found)
                return -1;
            const char *value = colon + 1;
            while (value < end && (*value == ' ' || *value == '\t'))
                ++value;
            size_t value_size = (size_t)(end - value);
            while (value_size > 0U && (value[value_size - 1U] == ' ' ||
                                       value[value_size - 1U] == '\t'))
                --value_size;
            if (value_size == 3U && strncasecmp(value, "m3u", value_size) == 0)
                *format = AUX_FORMAT_M3U;
            else if (value_size == 4U && strncasecmp(value, "m3u8", value_size) == 0)
                *format = AUX_FORMAT_M3U8;
            else if (value_size == 3U && strncasecmp(value, "pls", value_size) == 0)
                *format = AUX_FORMAT_PLS;
            else if (value_size == 4U && strncasecmp(value, "xspf", value_size) == 0)
                *format = AUX_FORMAT_XSPF;
            else if (value_size == 3U && strncasecmp(value, "asx", value_size) == 0)
                *format = AUX_FORMAT_ASX;
            else
                return -1;
            found = 1;
        }
        line = end + 2;
    }
    return 0;
}

static int aux_store_list(int connection, char *request, size_t received, size_t header_size)
{
    if (strncmp(request, "POST /list ", 11) != 0)
        return aux_send_text(connection, "404 Not Found", "Ruta no encontrada");

    size_t body_size = 0;
    unsigned char format = AUX_FORMAT_AUTO;
    const int length_result = parse_content_length(request, header_size, &body_size);
    if (length_result == 1)
        return aux_send_text(connection, "411 Length Required", "Falta Content-Length");
    if (length_result != 0 || body_size == 0)
        return aux_send_text(connection, "400 Bad Request", "Longitud de lista invalida o vacia");
    if (parse_aux_format(request, header_size, &format) != 0)
        return aux_send_text(connection, "400 Bad Request", "Formato de lista no compatible");

    size_t initial_body = received - header_size;
    if (initial_body > body_size)
        return aux_send_text(connection, "400 Bad Request", "Longitud de lista invalida");

    /* HTTP uploads and RPC memory-buffer writes commit to the same durable
     * file. Distinct temporary names preserve atomic rename without allowing
     * one producer to truncate the other's in-flight upload. */
    char temporary[sizeof(AUX_LIST_PATH) + 10];
    int temporary_size = snprintf(temporary, sizeof(temporary), "%s.http.tmp", AUX_LIST_PATH);
    if (temporary_size <= 0 || (size_t)temporary_size >= sizeof(temporary))
        return aux_send_text(connection, "500 Internal Server Error", "Error de ruta");
    int output = open(temporary, O_WRONLY | O_CREAT | O_TRUNC, 0644);
    if (output < 0)
        return aux_send_text(connection, "500 Internal Server Error", "No se pudo crear la lista");

    static const unsigned char envelope_magic[8] = {'P', 'R', 'A', 'U', 'X', '0', '1', '\n'};
    const unsigned char envelope_format = format;
    int ok = write_all(output, envelope_magic, sizeof(envelope_magic)) == 0 &&
             write_all(output, &envelope_format, sizeof(envelope_format)) == 0 &&
             (initial_body == 0 || write_all(output, request + header_size, initial_body) == 0);
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
    report("AUX playlist stored in /data/radio bytes=%lu format=%u",
           (unsigned long)body_size, (unsigned)format);
    return aux_send_text(connection, "201 Created", "Lista recibida; pulsa BARRIDO");
}

static void handle_aux_client(int connection)
{
    struct timeval timeout = {10, 0};
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
        ".brand{margin:0;color:#f0eee8;font-size:clamp(22px,4vw,32px);letter-spacing:.1em;text-shadow:0 1px 2px #000}.model{color:#f0eee8;font-size:12px;letter-spacing:.14em;text-transform:uppercase;text-shadow:0 1px 2px #202326}"
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
        "<section class='import'><div class='aux-id'>AUX / NETWORK BRIDGE · 7000</div><h2>Importar lista de emisoras</h2><p>Formatos M3U, M3U8, PLS, XSPF y ASX; después explora la lista en BARRIDO.</p>"
        "<label class='upload' for='file'><input id='file' type='file' accept='.m3u,.m3u8,.pls,.xspf,.asx'><b>SELECCIONAR LISTA DE EMISORAS</b><span>Admite M3U · M3U8 · PLS · XSPF · ASX</span></label>"
        "<div id='file-state' class='file-state'>Ningún archivo seleccionado</div><details><summary class='paste'>O pegar el contenido de una lista</summary>"
        "<textarea id='text' spellcheck='false' placeholder='Pega el contenido M3U, PLS, XSPF o ASX'></textarea></details>"
        "<div class='actions'><button id='send' type='button'>ENVIAR A PROSPERO RADIO</button><span id='status' class='status' role='status' aria-live='polite'></span></div></section></div></section></main>"
        "<script>(()=>{const file=document.querySelector('#file'),text=document.querySelector('#text'),label=document.querySelector('#file-state'),status=document.querySelector('#status'),button=document.querySelector('#send'),supported=['m3u','m3u8','pls','xspf','asx'];"
        "const guess=s=>{const v=s.toLowerCase();return v.includes('<asx')?'asx':(v.includes('xspf.org/ns')||v.includes('<tracklist'))?'xspf':(v.includes('[playlist]')||v.includes('file1='))?'pls':v.includes('#extm3u')?'m3u8':'m3u'};"
        "file.addEventListener('change',()=>{const f=file.files&&file.files[0];if(!f){label.textContent='Ningún archivo seleccionado';return}const ext=f.name.toLowerCase().split('.').pop();"
        "label.textContent=f.name+' · '+Math.ceil(f.size/1024)+' KB'+(supported.includes(ext)?'':' · extensión no compatible');status.textContent=''});"
        "button.addEventListener('click',async()=>{const f=file.files&&file.files[0];let body,format='auto';try{if(f){format=f.name.toLowerCase().split('.').pop();if(!supported.includes(format))throw new Error('format');body=await f.arrayBuffer()}"
        "else{const value=text.value;if(!value.trim()){status.className='status error';status.textContent='Selecciona un archivo o pega el contenido';return}format=guess(value);body=new TextEncoder().encode(value).buffer}}catch(e){status.className='status error';status.textContent='Formato o archivo no compatible';return}"
        "if(!body.byteLength){status.className='status error';status.textContent='La lista está vacía';return}"
        "button.disabled=true;status.className='status';status.textContent='Enviando lista '+format.toUpperCase()+'…';try{const r=await fetch('/list',{method:'POST',headers:{'Content-Type':'application/octet-stream','X-Playlist-Format':format},body});const answer=await r.text();"
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

static void *aux_worker_main(void *unused)
{
    (void)unused;
    for (;;)
    {
        int connection = -1;
        int pending[AUX_HTTP_QUEUE_CAPACITY];
        size_t pending_count = 0U;
        pthread_mutex_lock(&g_aux_worker_mutex);
        while (!g_aux_worker_shutdown && g_aux_http_queue_count == 0U)
            pthread_cond_wait(&g_aux_worker_condition, &g_aux_worker_mutex);
        if (g_aux_worker_shutdown)
        {
            while (g_aux_http_queue_count > 0U)
            {
                pending[pending_count++] =
                    g_aux_http_queue[g_aux_http_queue_head];
                g_aux_http_queue_head =
                    (g_aux_http_queue_head + 1U) % AUX_HTTP_QUEUE_CAPACITY;
                --g_aux_http_queue_count;
            }
            pthread_mutex_unlock(&g_aux_worker_mutex);
            for (size_t index = 0U; index < pending_count; ++index)
                close(pending[index]);
            break;
        }
        connection = g_aux_http_queue[g_aux_http_queue_head];
        g_aux_http_queue_head =
            (g_aux_http_queue_head + 1U) % AUX_HTTP_QUEUE_CAPACITY;
        --g_aux_http_queue_count;
        pthread_mutex_unlock(&g_aux_worker_mutex);
        handle_aux_client(connection);
    }
    return NULL;
}

static int aux_worker_start(void)
{
    pthread_mutex_lock(&g_aux_worker_mutex);
    g_aux_worker_shutdown = 0;
    g_aux_http_queue_head = 0U;
    g_aux_http_queue_count = 0U;
    pthread_mutex_unlock(&g_aux_worker_mutex);
    const int result = pthread_create(&g_aux_worker_thread, NULL, aux_worker_main, NULL);
    if (result != 0)
    {
        report("AUX HTTP worker start failed error=%d", result);
        return -1;
    }
    pthread_mutex_lock(&g_aux_worker_mutex);
    g_aux_worker_started = 1;
    pthread_mutex_unlock(&g_aux_worker_mutex);
    report("AUX HTTP worker started; RPC loop remains responsive during uploads");
    return 0;
}

static void aux_worker_stop(void)
{
    int pending[AUX_HTTP_QUEUE_CAPACITY];
    size_t pending_count = 0U;
    pthread_mutex_lock(&g_aux_worker_mutex);
    if (!g_aux_worker_started)
    {
        pthread_mutex_unlock(&g_aux_worker_mutex);
        return;
    }
    g_aux_worker_shutdown = 1;
    while (g_aux_http_queue_count > 0U)
    {
        pending[pending_count++] = g_aux_http_queue[g_aux_http_queue_head];
        g_aux_http_queue_head =
            (g_aux_http_queue_head + 1U) % AUX_HTTP_QUEUE_CAPACITY;
        --g_aux_http_queue_count;
    }
    pthread_cond_broadcast(&g_aux_worker_condition);
    pthread_mutex_unlock(&g_aux_worker_mutex);
    for (size_t index = 0U; index < pending_count; ++index)
        close(pending[index]);
    const int result = pthread_join(g_aux_worker_thread, NULL);
    if (result != 0)
        report("AUX HTTP worker join failed error=%d", result);
    pthread_mutex_lock(&g_aux_worker_mutex);
    g_aux_worker_started = 0;
    pthread_mutex_unlock(&g_aux_worker_mutex);
    report("AUX HTTP worker stopped");
}

static int aux_worker_enqueue_http(int connection)
{
    pthread_mutex_lock(&g_aux_worker_mutex);
    if (!g_aux_worker_started || g_aux_worker_shutdown ||
        g_aux_http_queue_count >= AUX_HTTP_QUEUE_CAPACITY)
    {
        pthread_mutex_unlock(&g_aux_worker_mutex);
        return -1;
    }
    const size_t tail =
        (g_aux_http_queue_head + g_aux_http_queue_count) % AUX_HTTP_QUEUE_CAPACITY;
    g_aux_http_queue[tail] = connection;
    ++g_aux_http_queue_count;
    pthread_cond_signal(&g_aux_worker_condition);
    pthread_mutex_unlock(&g_aux_worker_mutex);
    return 0;
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
    if (disc_worker_start() != 0)
        report("CD optical worker unavailable; radio bridge remains active");
    if (usb_worker_start() != 0)
        report("USB audio worker unavailable; radio bridge remains active");
    int aux_listener = -1;
    int disc_listener = -1;
    g_aux_ready = 0;
    g_aux_start_failed = 0;
    g_aux_queue_full_log_after = 0;
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
        if (disc_listener >= 0)
        {
            FD_SET(disc_listener, &readable);
            if (disc_listener > maximum_fd)
                maximum_fd = disc_listener;
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
                struct timeval reject_timeout = {1, 0};
                (void)setsockopt(client, SOL_SOCKET, SO_SNDTIMEO,
                                 &reject_timeout, sizeof(reject_timeout));
                if (aux_worker_enqueue_http(client) != 0)
                {
                    (void)aux_send_text(client, "503 Service Unavailable",
                                        "La entrada AUX esta ocupada; reintenta");
                    close(client);
                    const time_t now = time(NULL);
                    if (now >= g_aux_queue_full_log_after)
                    {
                        report("AUX clients rejected; bounded HTTP queue full");
                        g_aux_queue_full_log_after = now + 10;
                    }
                }
                last_activity = time(NULL);
            }
            else if (errno != EINTR)
                report("AUX accept failed errno=%d", errno);
        }
        if (selected > 0 && disc_listener >= 0 && FD_ISSET(disc_listener, &readable))
        {
            int client = accept(disc_listener, NULL, NULL);
            if (client >= 0)
            {
                struct timeval client_timeout = {3, 0};
                (void)setsockopt(client, SOL_SOCKET, SO_RCVTIMEO,
                                 &client_timeout, sizeof(client_timeout));
                (void)setsockopt(client, SOL_SOCKET, SO_SNDTIMEO,
                                 &client_timeout, sizeof(client_timeout));
                if (disc_worker_enqueue_http(client) != 0)
                {
                    (void)disc_http_error(client, "503 Service Unavailable",
                                          "CD ocupado; reintente\n");
                    close(client);
                }
                last_activity = time(NULL);
            }
            else if (errno != EINTR)
                report("DISC accept failed errno=%d", errno);
        }
        if (selected > 0 && bridge_client >= 0 && FD_ISSET(bridge_client, &readable))
        {
            int result = serve_bridge_request(bridge_client, &stop_requested, &aux_listener,
                                              &disc_listener);
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
    {
        close(aux_listener);
        aux_listener = -1;
    }
    g_aux_ready = 0;
    if (disc_listener >= 0)
        close(disc_listener);
    close(rpc_listener);
    aux_worker_stop();
    usb_worker_stop();
    disc_worker_stop();
    disc_close_stream();
    disc_unmount_owned_media();
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
    const char *aux_status = g_aux_ready ? "READY"
                             : g_aux_start_failed ? "FAILED"
                                                  : "STOPPED";
    report("process exit result=%d data_dir=%s data_rw=%s download0_rw=%s aux=%s",
           result, g_data_directory_ok ? "PASS" : "FAIL",
           g_data_roundtrip_ok ? "PASS" : "FAIL",
           g_download_roundtrip_ok ? "PASS" : "FAIL",
           aux_status);
    return result == 0 ? 0 : 1;
}
