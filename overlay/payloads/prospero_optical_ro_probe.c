// Prospero Radio - read-only optical media mount/read diagnostic payload.
// Copyright (C) 2026 BlackBearReloaded
// SPDX-License-Identifier: GPL-3.0-or-later

#include <sys/types.h>
#include <sys/cdio.h>
#include <sys/mount.h>
#include <sys/stat.h>
#include <sys/uio.h>

#include <stdio.h>
#include <cam/cam.h>
#include <cam/cam_ccb.h>
#include <cam/scsi/scsi_all.h>
#include <cam/scsi/scsi_pass.h>
#include <ps5/klog.h>

#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <stdint.h>
#include <stdarg.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ioctl.h>
#include <time.h>
#include <unistd.h>

#if defined(INTERNAL_DEVICE_BUILD)
#define DEVICE_PATH "/dev/cd0"
#define PROBE_NAME "INTERNAL-OPTICAL-RO"
#else
#ifndef DEVICE_PATH
#define DEVICE_PATH "/dev/cd1"
#endif
#ifndef PROBE_NAME
#define PROBE_NAME "USB-OPTICAL-RO"
#endif
#endif
#define MAX_TOC_TRACKS 99U
#define MAX_DIR_ENTRIES 96U
#define MAX_SCAN_DEPTH 3U
#define MAX_NAME_BYTES 128U
#define READ_BYTES 4096U
#define MAX_MOUNT_PATH_BYTES 128U
#define AUDIO_BYTES_PER_SECTOR 2352U
#define AUDIO_SAMPLE_SECTORS 3U
#define MAX_MEDIA_PATH_LOGS 12U
#define CDROM_LEADOUT_TRACK 0xaaU
#define UDF_LOGICAL_BLOCK_SIZE 2048U
#define UDF_MAX_VDS_BLOCKS 128U
#define UDF_MAX_DIRECTORY_BYTES (128U * UDF_LOGICAL_BLOCK_SIZE)
#define UDF_MAX_VAT_BYTES (8U * 1024U * 1024U)
#define UDF_MAX_DIRECTORY_ENTRIES 512U
#define UDF_MAX_DIRECTORY_DEPTH 8U
#define UDF_MAX_VISITED_DIRECTORIES 128U
#define UDF_MAX_NAME_BYTES 128U
#define UDF_MAX_PATH_BYTES 1024U
#define UDF_SAMPLE_PREFIX_BYTES (16U * 1024U)
#define UDF_MAX_ENTRY_PATH_LOGS 16U

struct track_info {
    uint8_t number;
    uint8_t control;
    uint32_t start_lba;
    uint32_t end_lba;
    int valid;
};

static int g_pass_fd = -1;
static path_id_t g_cam_path_id;
static target_id_t g_cam_target_id;
static lun_id_t g_cam_target_lun;
static int g_unit_ready;
static uint16_t g_current_profile;
static int g_profile_valid;
static size_t g_entries_seen;
static size_t g_files_seen;
static size_t g_media_path_logs;
static int g_media_sample_attempted;
static int g_media_sample_read;
static size_t g_media_counts[9];
static size_t g_last_data_transfer_length;
static char g_mount_path[MAX_MOUNT_PATH_BYTES];
static uint32_t g_capacity_last_lba;
static int g_capacity_valid;

static const char *const g_media_extensions[9] = {
    ".mp3", ".wma", ".wav", ".m4a", ".mp4a", ".aac", ".flac", ".ogg", ".opus"
};

static void report(const char *format, ...)
{
    char message[760];
    va_list args;
    va_start(args, format);
    int length = vsnprintf(message, sizeof(message), format, args);
    va_end(args);
    if (length < 0)
        return;
    message[sizeof(message) - 1U] = '\0';
    klog_printf("[" PROBE_NAME "] %s\n", message);
}

static uint32_t read_be32(const uint8_t *bytes)
{
    return ((uint32_t)bytes[0] << 24) | ((uint32_t)bytes[1] << 16) |
           ((uint32_t)bytes[2] << 8) | (uint32_t)bytes[3];
}

static uint16_t read_le16(const uint8_t *bytes)
{
    return (uint16_t)((uint16_t)bytes[0] | ((uint16_t)bytes[1] << 8));
}

static uint32_t read_le32(const uint8_t *bytes)
{
    return (uint32_t)bytes[0] | ((uint32_t)bytes[1] << 8) |
           ((uint32_t)bytes[2] << 16) | ((uint32_t)bytes[3] << 24);
}

static uint64_t read_le64(const uint8_t *bytes)
{
    uint64_t value = 0;
    for (size_t i = 0; i < 8U; ++i)
        value |= (uint64_t)bytes[i] << (i * 8U);
    return value;
}

static uint32_t fnv1a32(const uint8_t *bytes, size_t size)
{
    uint32_t hash = 2166136261U;
    for (size_t i = 0; i < size; ++i) {
        hash ^= bytes[i];
        hash *= 16777619U;
    }
    return hash;
}

static void hex_bytes(const uint8_t *bytes, size_t size, char *output, size_t capacity)
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

static int media_extension_index(const char *name)
{
    if (name == NULL)
        return -1;
    size_t name_length = strlen(name);
    for (size_t index = 0; index < sizeof(g_media_extensions) / sizeof(g_media_extensions[0]); ++index) {
        size_t extension_length = strlen(g_media_extensions[index]);
        if (name_length < extension_length)
            continue;
        const char *suffix = name + name_length - extension_length;
        size_t offset = 0;
        while (offset < extension_length) {
            char value = suffix[offset];
            if (value >= 'A' && value <= 'Z')
                value = (char)(value - 'A' + 'a');
            if (value != g_media_extensions[index][offset])
                break;
            ++offset;
        }
        if (offset == extension_length)
            return (int)index;
    }
    return -1;
}

static int open_pass_device(int cd_fd)
{
    union ccb ccb;
    memset(&ccb, 0, sizeof(ccb));
    ccb.ccb_h.func_code = XPT_GDEVLIST;
    if (ioctl(cd_fd, CAMGETPASSTHRU, &ccb) != 0 ||
        ccb.cgdl.status == CAM_GDEVLIST_ERROR) {
        int saved_errno = errno;
        report("CAMGETPASSTHRU device=%s result=FAIL errno=%d", DEVICE_PATH,
               saved_errno);
        return -1;
    }

    char peripheral[DEV_IDLEN + 1U];
    memcpy(peripheral, ccb.cgdl.periph_name, DEV_IDLEN);
    peripheral[DEV_IDLEN] = '\0';
    char path[96];
    int count = snprintf(path, sizeof(path), "/dev/%s%u", peripheral,
                         ccb.cgdl.unit_number);
    if (count <= 0 || (size_t)count >= sizeof(path)) {
        report("CAMGETPASSTHRU returned invalid path");
        return -1;
    }
    report("CAMGETPASSTHRU pass=%s", path);

    /* This kernel denies O_RDONLY on pass(4), even for CAM read commands.
     * The CDB allowlist below is the only path to CAMIOCOMMAND. */
    int fd = open(path, O_RDWR);
    if (fd < 0) {
        int saved_errno = errno;
        report("open pass=%s result=FAIL errno=%d (%s)", path,
               saved_errno, strerror(saved_errno));
        return -1;
    }

    memset(&ccb, 0, sizeof(ccb));
    ccb.ccb_h.func_code = XPT_GDEVLIST;
    if (ioctl(fd, CAMGETPASSTHRU, &ccb) != 0 ||
        ccb.cgdl.status == CAM_GDEVLIST_ERROR) {
        int saved_errno = errno;
        report("CAMGETPASSTHRU pass=%s result=FAIL errno=%d", path, saved_errno);
        close(fd);
        return -1;
    }
    g_cam_path_id = ccb.ccb_h.path_id;
    g_cam_target_id = ccb.ccb_h.target_id;
    g_cam_target_lun = ccb.ccb_h.target_lun;
    report("CAM_TARGET path=%s tuple=%u:%u:%llu", path,
           (unsigned int)g_cam_path_id, (unsigned int)g_cam_target_id,
           (unsigned long long)g_cam_target_lun);
    return fd;
}

static int issue_scsi(const char *name, const uint8_t *cdb, uint8_t cdb_length,
                      uint8_t *data, size_t data_length)
{
    if (cdb == NULL || cdb_length == 0 || cdb_length > CAM_MAX_CDBLEN ||
        data_length > UINT32_MAX)
        return -1;
    switch (cdb[0]) {
    case 0x00U: /* TEST UNIT READY */
        if (data_length == 0)
            break;
        return -1;
    case 0x12U: /* INQUIRY */
    case 0x25U: /* READ CAPACITY(10) */
    case 0x28U: /* READ(10) */
    case 0x43U: /* READ TOC/PMA/ATIP */
    case 0x46U: /* GET CONFIGURATION */
    case 0x51U: /* READ DISC INFORMATION */
    case 0x52U: /* READ TRACK INFORMATION */
    case 0xbeU: /* READ CD */
        break;
    default:
        report("SCSI_BLOCKED name=%s opcode=%#x (not on read-only allowlist)",
               name, (unsigned int)cdb[0]);
        return -1;
    }
    if (g_pass_fd < 0)
        return -1;

    char cdb_text[96];
    hex_bytes(cdb, cdb_length, cdb_text, sizeof(cdb_text));
    report("SCSI_BEGIN name=%s cdb=%s dxfer=%u", name, cdb_text,
           (unsigned int)data_length);

    union ccb ccb;
    memset(&ccb, 0, sizeof(ccb));
    ccb.ccb_h.path_id = g_cam_path_id;
    ccb.ccb_h.target_id = g_cam_target_id;
    ccb.ccb_h.target_lun = g_cam_target_lun;
    ccb.ccb_h.flags = data_length == 0 ? CAM_DIR_NONE : CAM_DIR_IN;
    ccb.ccb_h.flags |= CAM_DEV_QFRZDIS;
    memcpy(ccb.csio.cdb_io.cdb_bytes, cdb, cdb_length);
    cam_fill_csio(&ccb.csio, 0, NULL, ccb.ccb_h.flags, CAM_TAG_ACTION_NONE,
                  data, (uint32_t)data_length,
                  (uint8_t)sizeof(ccb.csio.sense_data), cdb_length, 30000U);
    ccb.ccb_h.path_id = g_cam_path_id;
    ccb.ccb_h.target_id = g_cam_target_id;
    ccb.ccb_h.target_lun = g_cam_target_lun;

    int result = ioctl(g_pass_fd, CAMIOCOMMAND, &ccb);
    int saved_errno = result == 0 ? 0 : errno;
    uint32_t cam_status = ccb.ccb_h.status & CAM_STATUS_MASK;
    uint8_t sense[24];
    size_t sense_size = ccb.csio.sense_len;
    if (sense_size > sizeof(sense))
        sense_size = sizeof(sense);
    memcpy(sense, &ccb.csio.sense_data, sense_size);
    char sense_text[3U * sizeof(sense) + 1U];
    hex_bytes(sense, sense_size, sense_text, sizeof(sense_text));
    report("SCSI_END name=%s ioctl=%s errno=%d cam=%#x scsi=%#x resid=%u sense=%s",
           name, result == 0 ? "OK" : "FAIL", saved_errno, cam_status,
           (unsigned int)ccb.csio.scsi_status, (unsigned int)ccb.csio.resid,
           sense_size == 0 ? "none" : sense_text);
    if (sense_size >= 14U && (sense[0] & 0x7eU) == 0x70U) {
        report("SCSI_SENSE name=%s key=%#x asc=%#x ascq=%#x",
               name, (unsigned int)(sense[2] & 0x0fU),
               (unsigned int)sense[12], (unsigned int)sense[13]);
    } else if (sense_size >= 4U && (sense[0] & 0x7eU) == 0x72U) {
        report("SCSI_SENSE name=%s key=%#x asc=%#x ascq=%#x",
               name, (unsigned int)(sense[1] & 0x0fU),
               (unsigned int)sense[2], (unsigned int)sense[3]);
    }
    if (ccb.csio.resid <= data_length)
        g_last_data_transfer_length = data_length - ccb.csio.resid;
    else
        g_last_data_transfer_length = 0;
    int transfer_valid = ccb.csio.resid == 0 ||
                         (cdb[0] == 0x43U && g_last_data_transfer_length >= 4U);
    return result == 0 && cam_status == CAM_REQ_CMP &&
                   ccb.csio.scsi_status == 0 && transfer_valid
               ? 0
               : -1;
}

static void scsi_basics(void)
{
    uint8_t inquiry_cdb[6] = {0x12U, 0, 0, 0, 96U, 0};
    uint8_t inquiry[96];
    memset(inquiry, 0, sizeof(inquiry));
    if (issue_scsi("INQUIRY", inquiry_cdb, sizeof(inquiry_cdb),
                    inquiry, sizeof(inquiry)) == 0) {
        report("INQUIRY vendor='%.*s' product='%.*s' revision='%.*s'",
               8, (const char *)&inquiry[8], 16, (const char *)&inquiry[16],
               4, (const char *)&inquiry[32]);
    }

    uint8_t ready_cdb[6] = {0};
    g_unit_ready = issue_scsi("TEST_UNIT_READY", ready_cdb, sizeof(ready_cdb), NULL, 0) == 0;
    report("MEDIA_READY result=%s", g_unit_ready ? "yes" : "no");

    uint8_t config_cdb[10] = {0x46U, 0x01U, 0, 0, 0, 0, 0, 0, 8U, 0};
    uint8_t config[8];
    memset(config, 0, sizeof(config));
    if (issue_scsi("GET_CONFIGURATION", config_cdb, sizeof(config_cdb),
                    config, sizeof(config)) == 0) {
        uint16_t profile = (uint16_t)(((uint16_t)config[6] << 8) | config[7]);
        g_current_profile = profile;
        g_profile_valid = 1;
        report("GET_CONFIGURATION profile=%#06x response_length=%u",
               (unsigned int)profile,
               (unsigned int)(((uint16_t)config[0] << 8) | config[1]));
    }
}

static void read_auxiliary_disc_metadata(uint8_t format, const char *name)
{
    uint8_t cdb[10] = {0x43U, 0, 0, 0, 0, 0, 0, 0, 0, 0};
    uint8_t data[804];
    memset(data, 0, sizeof(data));
    cdb[2] = format;
    cdb[7] = (uint8_t)(sizeof(data) >> 8);
    cdb[8] = (uint8_t)sizeof(data);
    if (issue_scsi(name, cdb, sizeof(cdb), data, sizeof(data)) != 0) {
        report("DISC_METADATA format=%u result=FAIL transferred=%u",
               (unsigned int)format, (unsigned int)g_last_data_transfer_length);
        return;
    }
    if (g_last_data_transfer_length < 4U) {
        report("DISC_METADATA format=%u result=SHORT transferred=%u",
               (unsigned int)format, (unsigned int)g_last_data_transfer_length);
        return;
    }
    size_t prefix_length = g_last_data_transfer_length;
    if (prefix_length > 32U)
        prefix_length = 32U;
    char prefix[3U * 32U + 1U];
    hex_bytes(data, prefix_length, prefix, sizeof(prefix));
    report("DISC_METADATA format=%u result=PASS transferred=%u declared=%u fnv1a32=%08x prefix=%s",
           (unsigned int)format, (unsigned int)g_last_data_transfer_length,
           (unsigned int)(((uint16_t)data[0] << 8) | data[1]),
           fnv1a32(data, g_last_data_transfer_length), prefix);
}

static void read_disc_information(void)
{
    uint8_t cdb[10] = {0x51U, 0, 0, 0, 0, 0, 0, 0, 34U, 0};
    uint8_t data[34];
    memset(data, 0, sizeof(data));
    if (issue_scsi("READ_DISC_INFORMATION", cdb, sizeof(cdb),
                   data, sizeof(data)) != 0) {
        report("DISC_INFORMATION result=FAIL");
        return;
    }
    char summary[3U * sizeof(data) + 1U];
    hex_bytes(data, sizeof(data), summary, sizeof(summary));
    report("DISC_INFORMATION result=PASS bytes=%u raw=%s",
           (unsigned int)sizeof(data), summary);
}

static int profile_is_dvd(void)
{
    if (!g_profile_valid)
        return 0;
    return (g_current_profile >= 0x0010U && g_current_profile <= 0x001bU) ||
           (g_current_profile >= 0x002aU && g_current_profile <= 0x002fU);
}

static void read_track_information(uint8_t track_number)
{
    uint8_t cdb[10] = {0x52U, 1U, 0, 0, 0, track_number, 0, 0, 48U, 0};
    uint8_t data[48];
    memset(data, 0, sizeof(data));
    if (issue_scsi("READ_TRACK_INFORMATION", cdb, sizeof(cdb),
                   data, sizeof(data)) != 0) {
        report("TRACK_INFORMATION track=%u result=FAIL transferred=%u",
               (unsigned int)track_number,
               (unsigned int)g_last_data_transfer_length);
        return;
    }
    char raw[3U * sizeof(data) + 1U];
    hex_bytes(data, sizeof(data), raw, sizeof(raw));
    report("TRACK_INFORMATION track=%u result=PASS bytes=%u declared=%u raw=%s",
           (unsigned int)track_number, (unsigned int)sizeof(data),
           (unsigned int)(((uint16_t)data[0] << 8) | data[1]), raw);
}

static int read_toc_ioctl(int cd_fd, struct track_info *tracks,
                          size_t *track_count, uint32_t *leadout)
{
    struct ioc_toc_header header;
    memset(&header, 0, sizeof(header));
    if (ioctl(cd_fd, CDIOREADTOCHEADER, &header) != 0) {
        int saved_errno = errno;
        report("CDIOREADTOCHEADER result=FAIL errno=%d (%s)", saved_errno,
               strerror(saved_errno));
        return -1;
    }
    if (header.starting_track == 0 || header.ending_track < header.starting_track ||
        (unsigned int)(header.ending_track - header.starting_track) >= MAX_TOC_TRACKS) {
        report("CDIOREADTOCHEADER invalid range first=%u last=%u",
               (unsigned int)header.starting_track, (unsigned int)header.ending_track);
        return -1;
    }

    size_t count = (size_t)(header.ending_track - header.starting_track) + 1U;
    memset(tracks, 0, sizeof(*tracks) * MAX_TOC_TRACKS);
    for (size_t i = 0; i < count; ++i) {
        struct ioc_read_toc_single_entry entry;
        memset(&entry, 0, sizeof(entry));
        entry.address_format = CD_LBA_FORMAT;
        entry.track = (uint8_t)(header.starting_track + i);
        if (ioctl(cd_fd, CDIOREADTOCENTRY, &entry) != 0) {
            int saved_errno = errno;
            report("CDIOREADTOCENTRY track=%u result=FAIL errno=%d",
                   (unsigned int)entry.track, saved_errno);
            continue;
        }
        tracks[i].number = entry.entry.track;
        tracks[i].control = (uint8_t)entry.entry.control;
        tracks[i].start_lba = read_be32(entry.entry.addr.addr);
        tracks[i].valid = 1;
    }

    struct ioc_read_toc_single_entry entry;
    memset(&entry, 0, sizeof(entry));
    entry.address_format = CD_LBA_FORMAT;
    entry.track = CDROM_LEADOUT_TRACK;
    if (ioctl(cd_fd, CDIOREADTOCENTRY, &entry) != 0) {
        int saved_errno = errno;
        report("CDIOREADTOCENTRY leadout result=FAIL errno=%d", saved_errno);
        return -1;
    }
    *leadout = read_be32(entry.entry.addr.addr);
    *track_count = count;
    return 0;
}

static int read_toc_scsi(struct track_info *tracks, size_t *track_count,
                         uint32_t *leadout)
{
    uint8_t cdb[10] = {0x43U, 0, 0, 0, 0, 0, 0, 0, 0, 0};
    uint8_t data[804];
    memset(data, 0, sizeof(data));
    cdb[7] = (uint8_t)(sizeof(data) >> 8);
    cdb[8] = (uint8_t)sizeof(data);
    if (issue_scsi("READ_TOC_FORMAT_0", cdb, sizeof(cdb), data, sizeof(data)) != 0)
        return -1;

    uint16_t response_length = (uint16_t)(((uint16_t)data[0] << 8) | data[1]);
    uint8_t first = data[2];
    uint8_t last = data[3];
    if ((size_t)response_length + 2U > g_last_data_transfer_length) {
        report("READ_TOC short response declared=%u transferred=%u",
               (unsigned int)response_length + 2U,
               (unsigned int)g_last_data_transfer_length);
        return -1;
    }
    if (response_length < 4U || first == 0 || last < first ||
        (unsigned int)(last - first) >= MAX_TOC_TRACKS) {
        report("READ_TOC invalid header length=%u first=%u last=%u",
               (unsigned int)response_length, (unsigned int)first, (unsigned int)last);
        return -1;
    }

    size_t count = (size_t)(last - first) + 1U;
    memset(tracks, 0, sizeof(*tracks) * MAX_TOC_TRACKS);
    size_t returned = ((size_t)response_length + 2U) / 8U;
    if (returned > (sizeof(data) - 4U) / 8U)
        returned = (sizeof(data) - 4U) / 8U;
    for (size_t i = 0; i < returned; ++i) {
        const uint8_t *entry = data + 4U + i * 8U;
        uint8_t number = entry[2];
        if (number == CDROM_LEADOUT_TRACK) {
            *leadout = read_be32(entry + 4U);
        } else if (number >= first && number <= last) {
            size_t index = (size_t)(number - first);
            tracks[index].number = number;
            tracks[index].control = (uint8_t)(entry[1] & 0x0fU);
            tracks[index].start_lba = read_be32(entry + 4U);
            tracks[index].valid = 1;
        }
    }
    if (*leadout == 0) {
        report("READ_TOC returned no leadout descriptor");
        return -1;
    }
    *track_count = count;
    return 0;
}

static void finish_track_bounds(struct track_info *tracks, size_t count,
                                uint32_t leadout)
{
    for (size_t i = 0; i < count; ++i) {
        if (!tracks[i].valid)
            continue;
        tracks[i].end_lba = leadout;
        for (size_t next = i + 1U; next < count; ++next) {
            if (tracks[next].valid) {
                tracks[i].end_lba = tracks[next].start_lba;
                break;
            }
        }
        report("TOC_TRACK track=%u control=%#x type=%s start=%u end=%u",
               (unsigned int)tracks[i].number, (unsigned int)tracks[i].control,
               (tracks[i].control & 0x04U) != 0 ? "data" : "audio",
               tracks[i].start_lba, tracks[i].end_lba);
    }
}

static int read_data_sector(const struct track_info *track)
{
    if (track->end_lba <= track->start_lba + 16U)
        return -1;
    uint32_t lba = track->start_lba + 16U;
    uint8_t cdb[10] = {0x28U, 0, 0, 0, 0, 0, 0, 0, 1U, 0};
    cdb[2] = (uint8_t)(lba >> 24);
    cdb[3] = (uint8_t)(lba >> 16);
    cdb[4] = (uint8_t)(lba >> 8);
    cdb[5] = (uint8_t)lba;
    uint8_t block[2048];
    memset(block, 0, sizeof(block));
    if (issue_scsi("READ10_DATA_SECTOR", cdb, sizeof(cdb),
                    block, sizeof(block)) != 0) {
        report("READ10_DATA result=FAIL track=%u lba=%u",
               (unsigned int)track->number, lba);
        return -1;
    }
    report("READ10_DATA result=PASS track=%u lba=%u bytes=%u fnv1a32=%08x",
           (unsigned int)track->number, lba, (unsigned int)sizeof(block),
           fnv1a32(block, sizeof(block)));
    return 0;
}

static void read_dvd_volume_sector(uint32_t lba, uint32_t last_lba)
{
    if (lba > last_lba) {
        report("DVD_READ10 skipped lba=%u capacity_last_lba=%u", lba, last_lba);
        return;
    }
    uint8_t cdb[10] = {0x28U, 0, 0, 0, 0, 0, 0, 0, 1U, 0};
    cdb[2] = (uint8_t)(lba >> 24);
    cdb[3] = (uint8_t)(lba >> 16);
    cdb[4] = (uint8_t)(lba >> 8);
    cdb[5] = (uint8_t)lba;
    uint8_t block[2048];
    memset(block, 0, sizeof(block));
    if (issue_scsi("READ10_DVD_VOLUME_SECTOR", cdb, sizeof(cdb),
                    block, sizeof(block)) != 0) {
        report("DVD_READ10 result=FAIL lba=%u", lba);
        return;
    }
    char prefix[3U * 32U + 1U];
    hex_bytes(block, 32U, prefix, sizeof(prefix));
    report("DVD_READ10 result=PASS lba=%u bytes=%u prefix32=%s fnv1a32=%08x",
           lba, (unsigned int)sizeof(block), prefix, fnv1a32(block, sizeof(block)));
    if (lba >= 16U && lba <= 31U) {
        char identifier[3U * 5U + 1U];
        hex_bytes(block + 1U, 5U, identifier, sizeof(identifier));
        report("DVD_VRS lba=%u type=%#04x identifier_hex=%s",
               lba, (unsigned int)block[0], identifier);
    }
}

static void probe_dvd_volume_sectors(void)
{
    uint8_t cdb[10] = {0x25U, 0, 0, 0, 0, 0, 0, 0, 0, 0};
    uint8_t data[8];
    memset(data, 0, sizeof(data));
    if (issue_scsi("READ_CAPACITY10", cdb, sizeof(cdb), data, sizeof(data)) != 0) {
        report("DVD_READ_CAPACITY result=FAIL");
        return;
    }
    uint32_t last_lba = read_be32(data);
    uint32_t block_size = read_be32(data + 4U);
    g_capacity_last_lba = last_lba;
    g_capacity_valid = 1;
    report("DVD_READ_CAPACITY result=PASS last_lba=%u block_size=%u",
           last_lba, block_size);
    if (block_size != 2048U) {
        report("DVD_VOLUME_SECTORS skipped block_size=%u expected=2048", block_size);
        return;
    }
    for (uint32_t lba = 16U; lba < 24U && lba <= last_lba; ++lba)
        read_dvd_volume_sector(lba, last_lba);
    for (uint32_t lba = 256U; lba < 264U && lba <= last_lba; ++lba)
        read_dvd_volume_sector(lba, last_lba);
    if (last_lba >= 256U)
        read_dvd_volume_sector(last_lba - 256U, last_lba);
}

struct udf_scan_context {
    uint32_t last_lba;
    uint32_t partition_start;
    uint32_t partition_length;
    uint16_t partition_number;
    uint16_t logical_partition_ref;
    uint16_t physical_partition_ref;
    int virtual_partition;
    uint8_t *vat_storage;
    const uint8_t *vat_entries;
    size_t vat_entry_count;
    uint32_t visited_lbn[UDF_MAX_VISITED_DIRECTORIES];
    size_t visited_count;
    size_t entries_seen;
    size_t directories_seen;
    size_t files_seen;
    size_t extension_counts[9];
    size_t entry_path_logs;
    size_t samples_read;
};

static int udf_tag_valid(const uint8_t *block, uint16_t expected_id)
{
    if (read_le16(block) != expected_id)
        return 0;
    uint8_t checksum = 0;
    for (size_t i = 0; i < 16U; ++i) {
        if (i != 4U)
            checksum = (uint8_t)(checksum + block[i]);
    }
    return checksum == block[4];
}

static uint16_t udf_crc_itu_t(const uint8_t *data, size_t length)
{
    uint16_t crc = 0U;
    for (size_t i = 0; i < length; ++i) {
        crc ^= (uint16_t)data[i] << 8;
        for (unsigned int bit = 0; bit < 8U; ++bit)
            crc = (crc & 0x8000U) != 0U ?
                (uint16_t)((crc << 1) ^ 0x1021U) :
                (uint16_t)(crc << 1);
    }
    return crc;
}

static void udf_report_tag_crc(const char *name, const uint8_t *block)
{
    uint16_t tag_id = read_le16(block);
    uint16_t crc_length = read_le16(block + 10U);
    uint16_t stored_crc = read_le16(block + 8U);
    if (crc_length > UDF_LOGICAL_BLOCK_SIZE - 16U) {
        report("UDF_DESCRIPTOR_CRC name=%s tag_id=%u length=%u result=OUT_OF_BLOCK",
               name, (unsigned int)tag_id, (unsigned int)crc_length);
        return;
    }
    uint16_t calculated_crc = udf_crc_itu_t(block + 16U, crc_length);
    report("UDF_DESCRIPTOR_CRC name=%s tag_id=%u length=%u stored=%04x calculated=%04x result=%s",
           name, (unsigned int)tag_id, (unsigned int)crc_length,
           (unsigned int)stored_crc, (unsigned int)calculated_crc,
           stored_crc == calculated_crc ? "valid" : "invalid");
}

static int udf_read_block(uint32_t lba, uint8_t *block, const char *name)
{
    if (!g_capacity_valid || lba > g_capacity_last_lba) {
        report("UDF_READ skipped lba=%u capacity_last_lba=%u valid=%s",
               lba, g_capacity_last_lba, g_capacity_valid ? "yes" : "no");
        return -1;
    }
    uint8_t cdb[10] = {0x28U, 0, 0, 0, 0, 0, 0, 0, 1U, 0};
    cdb[2] = (uint8_t)(lba >> 24);
    cdb[3] = (uint8_t)(lba >> 16);
    cdb[4] = (uint8_t)(lba >> 8);
    cdb[5] = (uint8_t)lba;
    memset(block, 0, UDF_LOGICAL_BLOCK_SIZE);
    if (issue_scsi(name, cdb, sizeof(cdb), block, UDF_LOGICAL_BLOCK_SIZE) != 0) {
        report("UDF_READ result=FAIL name=%s lba=%u", name, lba);
        return -1;
    }
    return 0;
}

static int udf_read_partition_block(struct udf_scan_context *context,
                                    uint32_t lbn, uint8_t *block,
                                    const char *name)
{
    uint32_t physical_lbn = lbn;
    if (context->virtual_partition) {
        if (context->vat_entries == NULL || (size_t)lbn >= context->vat_entry_count) {
            report("UDF_VAT_TRANSLATE skipped virtual_lbn=%u entries=%u",
                   lbn, (unsigned int)context->vat_entry_count);
            return -1;
        }
        physical_lbn = read_le32(context->vat_entries + (size_t)lbn * 4U);
        if (physical_lbn >= 0xfffffff0U) {
            report("UDF_VAT_TRANSLATE unallocated virtual_lbn=%u physical_lbn=%u",
                   lbn, physical_lbn);
            return -1;
        }
    }
    uint64_t absolute_lba = (uint64_t)context->partition_start + physical_lbn;
    if (physical_lbn >= context->partition_length || absolute_lba > context->last_lba) {
        report("UDF_PARTITION_READ skipped logical_lbn=%u physical_lbn=%u partition_length=%u absolute_lba=%llu",
               lbn, physical_lbn, context->partition_length,
               (unsigned long long)absolute_lba);
        return -1;
    }
    return udf_read_block((uint32_t)absolute_lba, block, name);
}

static int udf_read_partition_reference(struct udf_scan_context *context,
                                        uint16_t partition_ref, uint32_t lbn,
                                        uint8_t *block, const char *name)
{
    if (partition_ref == context->logical_partition_ref)
        return udf_read_partition_block(context, lbn, block, name);
    if (partition_ref == context->physical_partition_ref) {
        uint64_t absolute_lba = (uint64_t)context->partition_start + lbn;
        if (lbn >= context->partition_length || absolute_lba > context->last_lba) {
            report("UDF_PARTITION_READ skipped partition_ref=%u logical_lbn=%u partition_length=%u absolute_lba=%llu",
                   (unsigned int)partition_ref, lbn, context->partition_length,
                   (unsigned long long)absolute_lba);
            return -1;
        }
        return udf_read_block((uint32_t)absolute_lba, block, name);
    }
    report("UDF_PARTITION_READ skipped unsupported_partition_ref=%u logical_ref=%u physical_ref=%u lbn=%u",
           (unsigned int)partition_ref,
           (unsigned int)context->logical_partition_ref,
           (unsigned int)context->physical_partition_ref, lbn);
    return -1;
}

static void udf_decode_name(const uint8_t *source, size_t length,
                            char *destination, size_t capacity)
{
    if (capacity == 0)
        return;
    size_t written = 0;
    destination[0] = '\0';
    if (length == 0) {
        (void)snprintf(destination, capacity, "<empty>");
        return;
    }
    uint8_t compression = source[0];
    if (compression == 8U) {
        for (size_t i = 1; i < length && written + 1U < capacity; ++i) {
            uint8_t value = source[i];
            destination[written++] = value >= 0x20U && value <= 0x7eU ?
                                     (char)value : '?';
        }
    } else if (compression == 16U) {
        for (size_t i = 1; i + 1U < length && written + 1U < capacity; i += 2U) {
            uint16_t value = (uint16_t)(((uint16_t)source[i] << 8) | source[i + 1U]);
            destination[written++] = value >= 0x20U && value <= 0x7eU ?
                                     (char)value : '?';
        }
    } else {
        (void)snprintf(destination, capacity, "<compression-%u>",
                       (unsigned int)compression);
        return;
    }
    destination[written] = '\0';
    if (written == 0)
        (void)snprintf(destination, capacity, "<empty>");
}

static int udf_read_file_entry(struct udf_scan_context *context,
                               uint16_t partition_number, uint32_t lbn,
                               uint8_t *block, const char *name)
{
    if (partition_number != context->logical_partition_ref) {
        report("UDF_FILE_ENTRY skipped partition_ref=%u expected=%u lbn=%u",
               (unsigned int)partition_number,
               (unsigned int)context->logical_partition_ref, lbn);
        return -1;
    }
    if (udf_read_partition_reference(context, partition_number, lbn,
                                     block, name) != 0)
        return -1;
    uint16_t tag_id = read_le16(block);
    udf_report_tag_crc("file-entry", block);
    if ((tag_id != 261U && tag_id != 266U) || !udf_tag_valid(block, tag_id)) {
        report("UDF_FILE_ENTRY result=BAD_TAG lbn=%u tag_id=%u checksum=%s",
               lbn, (unsigned int)tag_id,
               udf_tag_valid(block, tag_id) ? "valid" : "invalid");
        return -1;
    }
    return 0;
}

struct udf_file_entry_layout {
    uint32_t base_length;
    uint32_t extended_attribute_offset;
    uint32_t allocation_length_offset;
};

static int udf_get_file_entry_layout(const uint8_t *file_entry,
                                     struct udf_file_entry_layout *layout)
{
    uint16_t tag_id = read_le16(file_entry);
    if (tag_id == 261U) {
        layout->base_length = 176U;
        layout->extended_attribute_offset = 168U;
        layout->allocation_length_offset = 172U;
        return 0;
    }
    if (tag_id == 266U) {
        layout->base_length = 216U;
        layout->extended_attribute_offset = 208U;
        layout->allocation_length_offset = 212U;
        return 0;
    }
    return -1;
}

static int udf_load_directory(struct udf_scan_context *context,
                              const uint8_t *file_entry, uint8_t **data_out,
                              size_t *size_out, const char *path)
{
    *data_out = NULL;
    *size_out = 0;
    struct udf_file_entry_layout layout;
    if (udf_get_file_entry_layout(file_entry, &layout) != 0) {
        report("UDF_DIRECTORY skipped unsupported_file_entry_tag=%u",
               (unsigned int)read_le16(file_entry));
        return -1;
    }
    uint64_t information_length = read_le64(file_entry + 56U);
    uint32_t extended_attribute_length = read_le32(
        file_entry + layout.extended_attribute_offset);
    uint32_t allocation_length = read_le32(
        file_entry + layout.allocation_length_offset);
    uint32_t allocation_offset = layout.base_length + extended_attribute_length;
    uint16_t icb_flags = read_le16(file_entry + 34U);
    unsigned int allocation_type = (unsigned int)(icb_flags & 0x07U);
    if (information_length == 0 || information_length > UDF_MAX_DIRECTORY_BYTES ||
        allocation_offset > UDF_LOGICAL_BLOCK_SIZE ||
        allocation_length > UDF_LOGICAL_BLOCK_SIZE - allocation_offset) {
        report("UDF_DIRECTORY skipped path=%s info_len=%llu ea_len=%u ad_len=%u",
               path, (unsigned long long)information_length,
               extended_attribute_length, allocation_length);
        return -1;
    }

    uint8_t *contents = malloc((size_t)information_length);
    if (contents == NULL) {
        report("UDF_DIRECTORY allocation_failed path=%s bytes=%llu",
               path, (unsigned long long)information_length);
        return -1;
    }
    size_t copied = 0;
    if (allocation_type == 3U) {
        if (information_length > UDF_LOGICAL_BLOCK_SIZE - allocation_offset) {
            free(contents);
            report("UDF_DIRECTORY embedded_too_large path=%s bytes=%llu",
                   path, (unsigned long long)information_length);
            return -1;
        }
        memcpy(contents, file_entry + allocation_offset, (size_t)information_length);
        copied = (size_t)information_length;
    } else if (allocation_type == 0U && allocation_length % 8U == 0U) {
        for (uint32_t offset = 0; offset < allocation_length &&
             copied < (size_t)information_length; offset += 8U) {
            uint32_t extent_word = read_le32(file_entry + allocation_offset + offset);
            uint32_t extent_bytes = extent_word & 0x3fffffffU;
            uint32_t extent_lbn = read_le32(file_entry + allocation_offset + offset + 4U);
            unsigned int extent_type = extent_word >> 30;
            if (extent_bytes == 0)
                continue;
            if (extent_type != 0U) {
                report("UDF_DIRECTORY unsupported_extent path=%s type=%u lbn=%u bytes=%u",
                       path, extent_type, extent_lbn, extent_bytes);
                free(contents);
                return -1;
            }
            uint64_t sectors = ((uint64_t)extent_bytes + UDF_LOGICAL_BLOCK_SIZE - 1U) /
                               UDF_LOGICAL_BLOCK_SIZE;
            if ((uint64_t)extent_lbn + sectors > context->partition_length) {
                report("UDF_DIRECTORY extent_out_of_bounds path=%s lbn=%u sectors=%llu",
                       path, extent_lbn, (unsigned long long)sectors);
                free(contents);
                return -1;
            }
            for (uint64_t sector = 0; sector < sectors &&
                 copied < (size_t)information_length; ++sector) {
                uint8_t block[UDF_LOGICAL_BLOCK_SIZE];
                if (udf_read_partition_block(context,
                        extent_lbn + (uint32_t)sector, block,
                        "UDF_READ_DIRECTORY") != 0) {
                    free(contents);
                    return -1;
                }
                size_t take = UDF_LOGICAL_BLOCK_SIZE;
                if (take > (size_t)information_length - copied)
                    take = (size_t)information_length - copied;
                memcpy(contents + copied, block, take);
                copied += take;
            }
        }
    } else if (allocation_type == 1U && allocation_length % 16U == 0U) {
        for (uint32_t offset = 0; offset < allocation_length &&
             copied < (size_t)information_length; offset += 16U) {
            uint32_t extent_word = read_le32(file_entry + allocation_offset + offset);
            uint32_t extent_bytes = extent_word & 0x3fffffffU;
            uint32_t extent_lbn = read_le32(file_entry + allocation_offset + offset + 4U);
            uint16_t extent_partition = read_le16(
                file_entry + allocation_offset + offset + 8U);
            unsigned int extent_type = extent_word >> 30;
            if (extent_bytes == 0U)
                continue;
            if (extent_type != 0U) {
                report("UDF_DIRECTORY unsupported_long_extent path=%s type=%u lbn=%u partition_ref=%u bytes=%u",
                       path, extent_type, extent_lbn,
                       (unsigned int)extent_partition, extent_bytes);
                free(contents);
                return -1;
            }
            uint64_t sectors = ((uint64_t)extent_bytes + UDF_LOGICAL_BLOCK_SIZE - 1U) /
                               UDF_LOGICAL_BLOCK_SIZE;
            if ((uint64_t)extent_lbn + sectors > context->partition_length) {
                report("UDF_DIRECTORY extent_out_of_bounds path=%s lbn=%u sectors=%llu",
                       path, extent_lbn, (unsigned long long)sectors);
                free(contents);
                return -1;
            }
            for (uint64_t sector = 0; sector < sectors &&
                 copied < (size_t)information_length; ++sector) {
                uint8_t block[UDF_LOGICAL_BLOCK_SIZE];
                if (udf_read_partition_reference(context, extent_partition,
                        extent_lbn + (uint32_t)sector, block,
                        "UDF_READ_DIRECTORY_LONG_AD") != 0) {
                    free(contents);
                    return -1;
                }
                size_t take = UDF_LOGICAL_BLOCK_SIZE;
                if (take > (size_t)information_length - copied)
                    take = (size_t)information_length - copied;
                memcpy(contents + copied, block, take);
                copied += take;
            }
        }
    } else {
        report("UDF_DIRECTORY unsupported_allocation path=%s type=%u ad_len=%u",
               path, allocation_type, allocation_length);
        free(contents);
        return -1;
    }
    if (copied != (size_t)information_length) {
        report("UDF_DIRECTORY short_read path=%s expected=%llu got=%u",
               path, (unsigned long long)information_length, (unsigned int)copied);
        free(contents);
        return -1;
    }
    *data_out = contents;
    *size_out = copied;
    report("UDF_DIRECTORY_READ result=PASS path=%s bytes=%u allocation_type=%u",
           path, (unsigned int)copied, allocation_type);
    size_t prefix_length = copied < 64U ? copied : 64U;
    char prefix[3U * 64U + 1U];
    hex_bytes(contents, prefix_length, prefix, sizeof(prefix));
    report("UDF_DIRECTORY_DATA path=%s bytes=%u prefix_hex=%s",
           path, (unsigned int)copied, prefix);
    return 0;
}

static int udf_copy_recorded_extent(struct udf_scan_context *context,
                                    uint16_t partition_ref, uint32_t extent_lbn,
                                    size_t extent_bytes, uint8_t *destination,
                                    size_t destination_capacity, size_t *copied,
                                    const char *path)
{
    size_t bytes_to_copy = extent_bytes;
    if (bytes_to_copy > destination_capacity - *copied)
        bytes_to_copy = destination_capacity - *copied;
    size_t offset = 0U;
    while (offset < bytes_to_copy) {
        uint64_t lbn = (uint64_t)extent_lbn +
                       offset / UDF_LOGICAL_BLOCK_SIZE;
        if (lbn > UINT32_MAX) {
            report("UDF_SAMPLE_READ stopped path=%s reason=lbn_overflow", path);
            return -1;
        }
        uint8_t block[UDF_LOGICAL_BLOCK_SIZE];
        if (udf_read_partition_reference(context, partition_ref, (uint32_t)lbn,
                                         block, "UDF_READ_AUDIO_PREFIX") != 0) {
            report("UDF_SAMPLE_READ stopped path=%s reason=sector_read", path);
            return -1;
        }
        size_t in_block = offset % UDF_LOGICAL_BLOCK_SIZE;
        size_t available = UDF_LOGICAL_BLOCK_SIZE - in_block;
        size_t take = bytes_to_copy - offset;
        if (take > available)
            take = available;
        memcpy(destination + *copied, block + in_block, take);
        *copied += take;
        offset += take;
    }
    return 0;
}

static int udf_is_golden_sample(const char *path)
{
    return strcmp(path, "01 Because you love me.mp3") == 0 ||
           strcmp(path, "Amaia Montero/Amaia Montero/01 Quiero Ser.m4a") == 0;
}

static void udf_probe_audio_file(struct udf_scan_context *context,
                                 uint16_t partition_number, uint32_t lbn,
                                 const char *path, int extension_index)
{
    if (!udf_is_golden_sample(path))
        return;
    uint8_t file_entry[UDF_LOGICAL_BLOCK_SIZE];
    if (udf_read_file_entry(context, partition_number, lbn, file_entry,
                            "UDF_READ_GOLDEN_FILE_ENTRY") != 0)
        return;
    uint8_t file_type = file_entry[27U];
    uint64_t information_length = read_le64(file_entry + 56U);
    struct udf_file_entry_layout layout;
    if (udf_get_file_entry_layout(file_entry, &layout) != 0 ||
        !udf_tag_valid(file_entry, read_le16(file_entry))) {
        report("UDF_SAMPLE_READ path=%s result=FAIL reason=invalid_file_entry", path);
        return;
    }
    uint32_t extended_attribute_length = read_le32(
        file_entry + layout.extended_attribute_offset);
    uint32_t allocation_length = read_le32(
        file_entry + layout.allocation_length_offset);
    uint32_t allocation_offset = layout.base_length + extended_attribute_length;
    unsigned int allocation_type =
        (unsigned int)(read_le16(file_entry + 34U) & 0x07U);
    if (file_type != 5U || information_length == 0U ||
        allocation_offset > UDF_LOGICAL_BLOCK_SIZE ||
        allocation_length > UDF_LOGICAL_BLOCK_SIZE - allocation_offset) {
        report("UDF_SAMPLE_READ path=%s result=FAIL reason=invalid_layout file_type=%u bytes=%llu allocation_type=%u ad_len=%u",
               path, (unsigned int)file_type,
               (unsigned long long)information_length, allocation_type,
               allocation_length);
        return;
    }

    size_t wanted = information_length < UDF_SAMPLE_PREFIX_BYTES ?
                    (size_t)information_length : UDF_SAMPLE_PREFIX_BYTES;
    uint8_t *sample = malloc(wanted);
    if (sample == NULL) {
        report("UDF_SAMPLE_READ path=%s result=FAIL reason=allocation bytes=%u",
               path, (unsigned int)wanted);
        return;
    }
    size_t copied = 0U;
    int read_result = 0;
    if (allocation_type == 3U) {
        if (wanted > UDF_LOGICAL_BLOCK_SIZE - allocation_offset) {
            read_result = -1;
        } else {
            memcpy(sample, file_entry + allocation_offset, wanted);
            copied = wanted;
        }
    } else if ((allocation_type == 0U && allocation_length % 8U == 0U) ||
               (allocation_type == 1U && allocation_length % 16U == 0U)) {
        const uint32_t stride = allocation_type == 1U ? 16U : 8U;
        for (uint32_t ad_offset = 0U;
             ad_offset < allocation_length && copied < wanted;
             ad_offset += stride) {
            const uint8_t *ad = file_entry + allocation_offset + ad_offset;
            uint32_t extent_word = read_le32(ad);
            uint32_t extent_bytes = extent_word & 0x3fffffffU;
            uint32_t extent_lbn = read_le32(ad + 4U);
            uint16_t extent_partition = allocation_type == 1U ?
                read_le16(ad + 8U) : context->logical_partition_ref;
            unsigned int extent_type = extent_word >> 30;
            if (extent_bytes == 0U)
                continue;
            if (extent_type != 0U) {
                report("UDF_SAMPLE_READ path=%s result=PARTIAL reason=unsupported_extent_type type=%u copied=%u",
                       path, extent_type, (unsigned int)copied);
                read_result = -1;
                break;
            }
            if (udf_copy_recorded_extent(context, extent_partition, extent_lbn,
                    extent_bytes, sample, wanted, &copied, path) != 0) {
                read_result = -1;
                break;
            }
        }
    } else {
        report("UDF_SAMPLE_READ path=%s result=FAIL reason=unsupported_allocation_type type=%u ad_len=%u",
               path, allocation_type, allocation_length);
        read_result = -1;
    }

    if (copied != wanted)
        read_result = -1;
    if (read_result == 0) {
        char prefix[3U * 32U + 1U];
        hex_bytes(sample, copied < 32U ? copied : 32U, prefix, sizeof(prefix));
        report("UDF_SAMPLE_READ path=%s result=PASS extension_index=%d file_bytes=%llu sample_bytes=%u fnv1a32=%08x prefix32=%s",
               path, extension_index, (unsigned long long)information_length,
               (unsigned int)copied, fnv1a32(sample, copied), prefix);
        ++context->samples_read;
    } else if (read_result != 0) {
        report("UDF_SAMPLE_READ path=%s result=FAIL extension_index=%d file_bytes=%llu sample_bytes=%u expected=%u",
               path, extension_index, (unsigned long long)information_length,
               (unsigned int)copied, (unsigned int)wanted);
    }
    free(sample);
}

static void udf_walk_directory(struct udf_scan_context *context,
                               uint16_t partition_number, uint32_t lbn,
                               const char *path, unsigned int depth)
{
    if (depth > UDF_MAX_DIRECTORY_DEPTH ||
        context->visited_count >= UDF_MAX_VISITED_DIRECTORIES ||
        context->entries_seen >= UDF_MAX_DIRECTORY_ENTRIES) {
        report("UDF_DIRECTORY_SCAN stopped path=%s depth=%u dirs=%u entries=%u",
               path[0] == '\0' ? "/" : path, depth,
               (unsigned int)context->visited_count,
               (unsigned int)context->entries_seen);
        return;
    }
    for (size_t i = 0; i < context->visited_count; ++i) {
        if (context->visited_lbn[i] == lbn) {
            report("UDF_DIRECTORY_SCAN cycle_skipped path=%s lbn=%u",
                   path[0] == '\0' ? "/" : path, lbn);
            return;
        }
    }
    context->visited_lbn[context->visited_count++] = lbn;

    uint8_t file_entry[UDF_LOGICAL_BLOCK_SIZE];
    if (udf_read_file_entry(context, partition_number, lbn, file_entry,
                            "UDF_READ_DIRECTORY_FILE_ENTRY") != 0)
        return;
    if (file_entry[27U] != 4U) {
        report("UDF_DIRECTORY_SCAN skipped path=%s file_type=%u",
               path[0] == '\0' ? "/" : path, (unsigned int)file_entry[27U]);
        return;
    }
    uint8_t *contents = NULL;
    size_t contents_length = 0;
    if (udf_load_directory(context, file_entry, &contents, &contents_length,
                           path[0] == '\0' ? "/" : path) != 0)
        return;

    size_t offset = 0;
    while (offset + 38U <= contents_length &&
           context->entries_seen < UDF_MAX_DIRECTORY_ENTRIES) {
        const uint8_t *fid = contents + offset;
        uint16_t tag_id = read_le16(fid);
        if (tag_id == 0U)
            break;
        if (tag_id != 257U || !udf_tag_valid(fid, 257U)) {
            report("UDF_FID invalid path=%s offset=%u tag_id=%u checksum=%s",
                   path[0] == '\0' ? "/" : path, (unsigned int)offset,
                   (unsigned int)tag_id,
                   udf_tag_valid(fid, tag_id) ? "valid" : "invalid");
            break;
        }
        uint8_t name_length = fid[19U];
        uint16_t implementation_length = read_le16(fid + 36U);
        size_t descriptor_length = 38U + (size_t)implementation_length + name_length;
        if (descriptor_length > contents_length - offset) {
            report("UDF_FID truncated path=%s offset=%u bytes=%u",
                   path[0] == '\0' ? "/" : path, (unsigned int)offset,
                   (unsigned int)descriptor_length);
            break;
        }
        size_t aligned_length = (descriptor_length + 3U) & ~3U;
        if (aligned_length > contents_length - offset)
            break;

        uint8_t file_characteristics = fid[18U];
        if ((file_characteristics & 0x0cU) == 0U && name_length != 0U) {
            char name[UDF_MAX_NAME_BYTES];
            char entry_path[UDF_MAX_PATH_BYTES];
            udf_decode_name(fid + 38U + implementation_length, name_length,
                            name, sizeof(name));
            int path_length = path[0] == '\0' ?
                snprintf(entry_path, sizeof(entry_path), "%s", name) :
                snprintf(entry_path, sizeof(entry_path), "%s/%s", path, name);
            if (path_length > 0 && (size_t)path_length < sizeof(entry_path)) {
                uint32_t child_lbn = read_le32(fid + 24U);
                uint16_t child_partition = read_le16(fid + 28U);
                int is_directory = (file_characteristics & 0x02U) != 0U;
                int extension_index = media_extension_index(name);
                ++context->entries_seen;
                if (is_directory) {
                    ++context->directories_seen;
                    if (depth < UDF_MAX_DIRECTORY_DEPTH)
                        udf_walk_directory(context, child_partition, child_lbn,
                                           entry_path, depth + 1U);
                } else {
                    ++context->files_seen;
                    if (extension_index >= 0) {
                        ++context->extension_counts[(size_t)extension_index];
                        udf_probe_audio_file(context, child_partition, child_lbn,
                                             entry_path, extension_index);
                    }
                }
                if (context->entry_path_logs < UDF_MAX_ENTRY_PATH_LOGS) {
                    report("UDF_ENTRY path=%s kind=%s lbn=%u partition_ref=%u extension_index=%d",
                           entry_path, is_directory ? "directory" : "file",
                           child_lbn, (unsigned int)child_partition,
                           extension_index);
                    ++context->entry_path_logs;
                }
            }
        }
        offset += aligned_length;
    }
    free(contents);
}

static int udf_read_physical_file(struct udf_scan_context *context,
                                  const uint8_t *file_entry,
                                  uint8_t **contents_out, size_t *length_out)
{
    *contents_out = NULL;
    *length_out = 0;
    struct udf_file_entry_layout layout;
    if (udf_get_file_entry_layout(file_entry, &layout) != 0)
        return -1;
    uint64_t information_length = read_le64(file_entry + 56U);
    uint32_t extended_attribute_length = read_le32(
        file_entry + layout.extended_attribute_offset);
    uint32_t allocation_length = read_le32(
        file_entry + layout.allocation_length_offset);
    uint32_t allocation_offset = layout.base_length + extended_attribute_length;
    unsigned int allocation_type = (unsigned int)(read_le16(file_entry + 34U) & 0x07U);
    report("UDF_VAT_FILE_LAYOUT tag_id=%u file_type=%u info_len=%llu allocation_type=%u base=%u ea_len=%u ad_len=%u",
           (unsigned int)read_le16(file_entry), (unsigned int)file_entry[27U],
           (unsigned long long)information_length, allocation_type,
           layout.base_length, extended_attribute_length, allocation_length);
    if (information_length == 0U || information_length > UDF_MAX_VAT_BYTES ||
        allocation_offset > UDF_LOGICAL_BLOCK_SIZE ||
        allocation_length > UDF_LOGICAL_BLOCK_SIZE - allocation_offset) {
        report("UDF_VAT_FILE skipped invalid_size bytes=%llu ea_len=%u ad_len=%u",
               (unsigned long long)information_length,
               extended_attribute_length, allocation_length);
        return -1;
    }

    uint8_t *contents = malloc((size_t)information_length);
    if (contents == NULL) {
        report("UDF_VAT_FILE allocation_failed bytes=%llu",
               (unsigned long long)information_length);
        return -1;
    }
    size_t copied = 0;
    if (allocation_type == 3U) {
        if (information_length > UDF_LOGICAL_BLOCK_SIZE - allocation_offset) {
            free(contents);
            report("UDF_VAT_FILE embedded_too_large bytes=%llu",
                   (unsigned long long)information_length);
            return -1;
        }
        report("UDF_VAT_FILE_STORAGE mode=embedded offset=%u bytes=%llu",
               allocation_offset, (unsigned long long)information_length);
        memcpy(contents, file_entry + allocation_offset, (size_t)information_length);
        copied = (size_t)information_length;
    } else if (allocation_type == 0U && allocation_length % 8U == 0U) {
        for (uint32_t offset = 0; offset < allocation_length &&
             copied < (size_t)information_length; offset += 8U) {
            uint32_t extent_word = read_le32(file_entry + allocation_offset + offset);
            uint32_t extent_bytes = extent_word & 0x3fffffffU;
            uint32_t extent_lbn = read_le32(file_entry + allocation_offset + offset + 4U);
            unsigned int extent_type = extent_word >> 30;
            if (extent_bytes == 0U)
                continue;
            report("UDF_VAT_FILE_EXTENT type=%u lbn=%u bytes=%u sectors=%llu",
                   extent_type, extent_lbn, extent_bytes,
                   (unsigned long long)(((uint64_t)extent_bytes +
                       UDF_LOGICAL_BLOCK_SIZE - 1U) / UDF_LOGICAL_BLOCK_SIZE));
            if (extent_type != 0U) {
                report("UDF_VAT_FILE unsupported_extent type=%u lbn=%u bytes=%u",
                       extent_type, extent_lbn, extent_bytes);
                free(contents);
                return -1;
            }
            uint64_t sectors = ((uint64_t)extent_bytes + UDF_LOGICAL_BLOCK_SIZE - 1U) /
                               UDF_LOGICAL_BLOCK_SIZE;
            if ((uint64_t)extent_lbn + sectors > context->partition_length) {
                report("UDF_VAT_FILE extent_out_of_bounds lbn=%u sectors=%llu",
                       extent_lbn, (unsigned long long)sectors);
                free(contents);
                return -1;
            }
            for (uint64_t sector = 0; sector < sectors &&
                 copied < (size_t)information_length; ++sector) {
                uint64_t absolute_lba = (uint64_t)context->partition_start +
                                        extent_lbn + sector;
                if (absolute_lba > context->last_lba) {
                    free(contents);
                    report("UDF_VAT_FILE read_out_of_bounds lba=%llu capacity_last_lba=%u",
                           (unsigned long long)absolute_lba, context->last_lba);
                    return -1;
                }
                uint8_t block[UDF_LOGICAL_BLOCK_SIZE];
                if (udf_read_block((uint32_t)absolute_lba, block,
                                   "UDF_READ_VAT_DATA") != 0) {
                    free(contents);
                    return -1;
                }
                size_t take = UDF_LOGICAL_BLOCK_SIZE;
                if (take > (size_t)information_length - copied)
                    take = (size_t)information_length - copied;
                memcpy(contents + copied, block, take);
                copied += take;
            }
        }
    } else {
        free(contents);
        report("UDF_VAT_FILE unsupported_allocation type=%u ad_len=%u",
               allocation_type, allocation_length);
        return -1;
    }
    if (copied != (size_t)information_length) {
        free(contents);
        report("UDF_VAT_FILE short_read expected=%llu got=%u",
               (unsigned long long)information_length, (unsigned int)copied);
        return -1;
    }
    *contents_out = contents;
    *length_out = copied;
    return 0;
}

static int udf_scan_fileset_descriptor(struct udf_scan_context *context,
                                       const uint8_t *fileset,
                                       uint32_t fsd_lbn,
                                       uint16_t fsd_partition,
                                       const char *vat_source)
{
    uint16_t tag_id = read_le16(fileset);
    udf_report_tag_crc("fileset", fileset);
    if (!udf_tag_valid(fileset, 256U)) {
        report("UDF_FILESET source=%s result=INVALID lbn=%u tag_id=%u checksum=%s",
               vat_source, fsd_lbn, (unsigned int)tag_id,
               udf_tag_valid(fileset, tag_id) ? "valid" : "invalid");
        return -1;
    }

    uint32_t root_length = read_le32(fileset + 400U) & 0x3fffffffU;
    uint32_t root_lbn = read_le32(fileset + 404U);
    uint16_t root_partition = read_le16(fileset + 408U);
    report("UDF_FILESET source=%s result=PASS lbn=%u root_lbn=%u root_partition=%u root_extent_bytes=%u",
           vat_source, fsd_lbn, root_lbn, (unsigned int)root_partition,
           root_length);
    if (root_partition != fsd_partition || root_lbn >= context->partition_length ||
        root_length < 176U) {
        report("UDF_ROOT source=%s stopped invalid_icb", vat_source);
        return -1;
    }

    uint8_t file_entry[UDF_LOGICAL_BLOCK_SIZE];
    if (udf_read_file_entry(context, root_partition, root_lbn, file_entry,
                            "UDF_READ_ROOT_FILE_ENTRY") != 0)
        return -1;
    struct udf_file_entry_layout layout;
    if (udf_get_file_entry_layout(file_entry, &layout) != 0) {
        report("UDF_ROOT source=%s stopped unsupported_file_entry", vat_source);
        return -1;
    }
    uint32_t extended_attribute_length = read_le32(
        file_entry + layout.extended_attribute_offset);
    uint32_t allocation_length = read_le32(
        file_entry + layout.allocation_length_offset);
    report("UDF_ROOT_FILE_ENTRY source=%s result=PASS lbn=%u file_type=%u info_len=%llu allocation_type=%u ea_len=%u ad_len=%u",
           vat_source, root_lbn, (unsigned int)file_entry[27U],
           (unsigned long long)read_le64(file_entry + 56U),
           (unsigned int)(read_le16(file_entry + 34U) & 0x07U),
           extended_attribute_length, allocation_length);
    if (file_entry[27U] != 4U) {
        report("UDF_ROOT source=%s stopped root_is_not_directory", vat_source);
        return -1;
    }

    udf_walk_directory(context, root_partition, root_lbn, "", 0U);
    report("UDF_SCAN source=%s result=COMPLETE entries=%u directories=%u directories_scanned=%u files=%u mp3=%u wma=%u wav=%u m4a=%u mp4a=%u aac=%u flac=%u ogg=%u opus=%u samples_read=%u path_logs=%u",
           vat_source, (unsigned int)context->entries_seen,
           (unsigned int)context->directories_seen,
           (unsigned int)context->visited_count,
           (unsigned int)context->files_seen,
           (unsigned int)context->extension_counts[0],
           (unsigned int)context->extension_counts[1],
           (unsigned int)context->extension_counts[2],
           (unsigned int)context->extension_counts[3],
           (unsigned int)context->extension_counts[4],
           (unsigned int)context->extension_counts[5],
           (unsigned int)context->extension_counts[6],
           (unsigned int)context->extension_counts[7],
           (unsigned int)context->extension_counts[8],
           (unsigned int)context->samples_read,
           (unsigned int)context->entry_path_logs);
    return 0;
}

static int udf_probe_previous_vat(struct udf_scan_context *context,
                                  uint32_t fsd_lbn,
                                  uint16_t fsd_partition)
{
    if (context->vat_storage == NULL || read_le16(context->vat_storage) < 136U) {
        report("UDF_PREVIOUS_VAT skipped current_vat_header_unavailable");
        return -1;
    }
    uint32_t previous_vat_lbn = read_le32(context->vat_storage + 132U);
    if (previous_vat_lbn == 0xffffffffU ||
        previous_vat_lbn >= context->partition_length) {
        report("UDF_PREVIOUS_VAT skipped invalid_lbn=%u partition_length=%u",
               previous_vat_lbn, context->partition_length);
        return -1;
    }
    uint64_t previous_vat_lba = previous_vat_lbn;
    if (previous_vat_lba > context->last_lba) {
        report("UDF_PREVIOUS_VAT skipped out_of_capacity lbn=%u lba=%llu last_lba=%u",
               previous_vat_lbn, (unsigned long long)previous_vat_lba,
               context->last_lba);
        return -1;
    }

    uint8_t file_entry[UDF_LOGICAL_BLOCK_SIZE];
    if (udf_read_block((uint32_t)previous_vat_lba, file_entry,
                       "UDF_READ_PREVIOUS_VAT_FILE_ENTRY") != 0)
        return -1;
    uint16_t file_entry_tag = read_le16(file_entry);
    udf_report_tag_crc("previous-vat", file_entry);
    if ((file_entry_tag != 261U && file_entry_tag != 266U) ||
        !udf_tag_valid(file_entry, file_entry_tag) || file_entry[27U] != 0xf8U) {
        report("UDF_PREVIOUS_VAT result=INVALID lbn=%u device_lba=%u tag_id=%u file_type=%u checksum=%s",
               previous_vat_lbn, (uint32_t)previous_vat_lba,
               (unsigned int)file_entry_tag, (unsigned int)file_entry[27U],
               udf_tag_valid(file_entry, file_entry_tag) ? "valid" : "invalid");
        return -1;
    }
    unsigned int allocation_type =
        (unsigned int)(read_le16(file_entry + 34U) & 0x07U);
    if (allocation_type != 3U) {
        report("UDF_PREVIOUS_VAT skipped allocation_type=%u lbn=%u",
               allocation_type, previous_vat_lbn);
        return -1;
    }

    uint8_t *previous_vat = NULL;
    size_t previous_vat_length = 0;
    if (udf_read_physical_file(context, file_entry, &previous_vat,
                               &previous_vat_length) != 0)
        return -1;
    if (previous_vat_length < 152U) {
        report("UDF_PREVIOUS_VAT result=INVALID short_header bytes=%u",
               (unsigned int)previous_vat_length);
        free(previous_vat);
        return -1;
    }

    uint16_t header_length = read_le16(previous_vat);
    uint16_t implementation_use_length = read_le16(previous_vat + 2U);
    if (header_length < 152U || header_length > previous_vat_length ||
        (previous_vat_length - header_length) % 4U != 0U) {
        report("UDF_PREVIOUS_VAT result=INVALID header=%u implementation_use=%u bytes=%u",
               (unsigned int)header_length,
               (unsigned int)implementation_use_length,
               (unsigned int)previous_vat_length);
        free(previous_vat);
        return -1;
    }
    size_t entry_count = (previous_vat_length - header_length) / 4U;
    if (entry_count == 0U || (size_t)fsd_lbn >= entry_count) {
        report("UDF_PREVIOUS_VAT result=INVALID entries=%u fsd_lbn=%u",
               (unsigned int)entry_count, fsd_lbn);
        free(previous_vat);
        return -1;
    }
    const uint8_t *previous_entries = previous_vat + header_length;
    report("UDF_PREVIOUS_VAT result=PASS lbn=%u device_lba=%u bytes=%u header=%u implementation_use=%u entries=%u fsd_physical_lbn=%u",
           previous_vat_lbn, (uint32_t)previous_vat_lba,
           (unsigned int)previous_vat_length, (unsigned int)header_length,
           (unsigned int)implementation_use_length, (unsigned int)entry_count,
           read_le32(previous_entries + (size_t)fsd_lbn * 4U));

    const uint8_t *saved_entries = context->vat_entries;
    size_t saved_entry_count = context->vat_entry_count;
    context->vat_entries = previous_entries;
    context->vat_entry_count = entry_count;
    context->virtual_partition = 1;
    context->visited_count = 0U;
    context->entries_seen = 0U;
    context->directories_seen = 0U;
    context->files_seen = 0U;
    memset(context->extension_counts, 0, sizeof(context->extension_counts));
    context->entry_path_logs = 0U;
    context->samples_read = 0U;

    uint8_t fileset[UDF_LOGICAL_BLOCK_SIZE];
    int result = -1;
    if (udf_read_partition_block(context, fsd_lbn, fileset,
                                 "UDF_READ_PREVIOUS_FILESET_DESCRIPTOR") == 0)
        result = udf_scan_fileset_descriptor(context, fileset, fsd_lbn,
                                             fsd_partition, "previous-vat");

    context->vat_entries = saved_entries;
    context->vat_entry_count = saved_entry_count;
    free(previous_vat);
    return result;
}

static int udf_load_virtual_allocation_table(struct udf_scan_context *context,
                                             uint16_t logical_partition_ref)
{
    for (uint32_t distance = 0; distance < 4U &&
         distance <= context->last_lba; ++distance) {
        uint32_t lba = context->last_lba - distance;
        uint8_t file_entry[UDF_LOGICAL_BLOCK_SIZE];
        if (udf_read_block(lba, file_entry, "UDF_READ_VAT_FILE_ENTRY") != 0)
            continue;
        uint16_t tag_id = read_le16(file_entry);
        if (tag_id == 261U || tag_id == 266U)
            udf_report_tag_crc("vat-file-entry", file_entry);
        if ((tag_id != 261U && tag_id != 266U) ||
            !udf_tag_valid(file_entry, tag_id)) {
            report("UDF_VAT_CANDIDATE lba=%u tag_id=%u result=not_file_entry",
                   lba, (unsigned int)tag_id);
            continue;
        }
        uint8_t file_type = file_entry[27U];
        report("UDF_VAT_CANDIDATE lba=%u tag_id=%u file_type=%u info_len=%llu",
               lba, (unsigned int)tag_id, (unsigned int)file_type,
               (unsigned long long)read_le64(file_entry + 56U));
        if (file_type != 0xf8U)
            continue;

        uint8_t *vat = NULL;
        size_t vat_length = 0;
        if (udf_read_physical_file(context, file_entry, &vat, &vat_length) != 0)
            return -1;
        if (vat_length < 152U) {
            report("UDF_VAT result=INVALID short_header bytes=%u",
                   (unsigned int)vat_length);
            free(vat);
            return -1;
        }
        uint16_t header_length = read_le16(vat);
        uint16_t implementation_use_length = read_le16(vat + 2U);
        if (header_length < 152U || header_length > vat_length ||
            (vat_length - header_length) % 4U != 0U) {
            report("UDF_VAT result=INVALID header=%u implementation_use=%u bytes=%u",
                   (unsigned int)header_length,
                   (unsigned int)implementation_use_length,
                   (unsigned int)vat_length);
            free(vat);
            return -1;
        }
        uint32_t entry_count = (uint32_t)((vat_length - header_length) / 4U);
        if (entry_count == 0U) {
            report("UDF_VAT result=INVALID no_entries");
            free(vat);
            return -1;
        }
        context->vat_storage = vat;
        context->vat_entries = vat + header_length;
        context->vat_entry_count = entry_count;
        context->logical_partition_ref = logical_partition_ref;
        context->virtual_partition = 1;
        report("UDF_VAT result=PASS physical_lba=%u bytes=%u header=%u implementation_use=%u entries=%u partition_start=%u entry0=%u previous_vat_lbn=%u",
               lba, (unsigned int)vat_length, (unsigned int)header_length,
               (unsigned int)implementation_use_length, entry_count,
               context->partition_start, read_le32(context->vat_entries),
               header_length >= 136U ? read_le32(vat + 132U) : 0U);
        size_t sample_entries = entry_count < 8U ? entry_count : 8U;
        for (size_t entry = 0; entry < sample_entries; ++entry)
            report("UDF_VAT_ENTRY virtual_lbn=%u physical_lbn=%u",
                   (unsigned int)entry,
                   read_le32(context->vat_entries + entry * 4U));
        return 0;
    }
    report("UDF_VAT result=NOT_FOUND scanned_last_blocks=4");
    return -1;
}

static void probe_udf_volume(uint32_t anchor_lba)
{
    if (!g_capacity_valid || anchor_lba > g_capacity_last_lba) {
        report("UDF_SCAN skipped capacity_unavailable");
        return;
    }
    uint8_t block[UDF_LOGICAL_BLOCK_SIZE];
    if (udf_read_block(anchor_lba, block, "UDF_READ_ANCHOR") != 0)
        return;
    uint16_t anchor_tag = read_le16(block);
    udf_report_tag_crc("anchor", block);
    if (anchor_tag != 2U || !udf_tag_valid(block, 2U)) {
        report("UDF_ANCHOR result=INVALID tag_id=%u checksum=%s",
               (unsigned int)anchor_tag,
               udf_tag_valid(block, anchor_tag) ? "valid" : "invalid");
        return;
    }

    uint32_t vds_bytes = read_le32(block + 16U) & 0x3fffffffU;
    uint32_t vds_start = read_le32(block + 20U);
    uint64_t vds_blocks = ((uint64_t)vds_bytes + UDF_LOGICAL_BLOCK_SIZE - 1U) /
                          UDF_LOGICAL_BLOCK_SIZE;
    report("UDF_ANCHOR result=PASS anchor_lba=%u main_vds_lba=%u bytes=%u blocks=%u tag_location=%u",
           anchor_lba, vds_start, vds_bytes, (unsigned int)vds_blocks,
           read_le32(block + 12U));
    if (vds_bytes == 0U || vds_blocks == 0U || vds_blocks > UDF_MAX_VDS_BLOCKS ||
        (uint64_t)vds_start + vds_blocks > (uint64_t)g_capacity_last_lba + 1U) {
        report("UDF_VDS skipped invalid_extent lba=%u bytes=%u blocks=%u",
               vds_start, vds_bytes, (unsigned int)vds_blocks);
        return;
    }

    int have_partition = 0;
    int have_logical_volume = 0;
    int matching_type1_map = 0;
    int matching_virtual_map = 0;
    int mapped_partition_number = 0;
    uint16_t partition_number = 0;
    uint16_t physical_partition_ref = 0xffffU;
    uint32_t partition_start = 0;
    uint32_t partition_length = 0;
    uint32_t fsd_lbn = 0;
    uint16_t fsd_partition = 0;
    for (uint64_t i = 0; i < vds_blocks; ++i) {
        uint32_t lba = vds_start + (uint32_t)i;
        if (udf_read_block(lba, block, "UDF_READ_VDS") != 0)
            return;
        uint16_t tag_id = read_le16(block);
        int tag_valid = udf_tag_valid(block, tag_id);
        udf_report_tag_crc("vds", block);
        report("UDF_VDS_DESCRIPTOR lba=%u tag_id=%u checksum=%s tag_location=%u",
               lba, (unsigned int)tag_id, tag_valid ? "valid" : "invalid",
               read_le32(block + 12U));
        if (tag_id == 8U)
            break;
        if (!tag_valid)
            continue;
        if (tag_id == 5U) {
            partition_number = read_le16(block + 22U);
            partition_start = read_le32(block + 188U);
            partition_length = read_le32(block + 192U);
            have_partition = 1;
            report("UDF_PARTITION number=%u start=%u length=%u end=%llu",
                   (unsigned int)partition_number, partition_start,
                   partition_length,
                   (unsigned long long)partition_start + partition_length);
        } else if (tag_id == 6U) {
            uint32_t logical_block_size = read_le32(block + 212U);
            uint32_t fsd_extent_bytes = read_le32(block + 248U) & 0x3fffffffU;
            fsd_lbn = read_le32(block + 252U);
            fsd_partition = read_le16(block + 256U);
            uint32_t map_length = read_le32(block + 264U);
            uint32_t map_count = read_le32(block + 268U);
            report("UDF_LOGICAL_VOLUME block_size=%u fsd_extent_bytes=%u fsd_lbn=%u fsd_partition=%u map_count=%u map_length=%u",
                   logical_block_size, fsd_extent_bytes, fsd_lbn,
                   (unsigned int)fsd_partition, map_count, map_length);
            if (logical_block_size != UDF_LOGICAL_BLOCK_SIZE ||
                map_length > UDF_LOGICAL_BLOCK_SIZE - 440U || map_count > 64U) {
                report("UDF_PARTITION_MAP skipped invalid_lvd_lengths");
                continue;
            }
            uint32_t map_offset = 440U;
            uint32_t consumed = 0;
            for (uint32_t map = 0; map < map_count && consumed < map_length; ++map) {
                uint8_t type = block[map_offset + consumed];
                uint8_t length = block[map_offset + consumed + 1U];
                if (length < 2U || length > map_length - consumed) {
                    report("UDF_PARTITION_MAP invalid map=%u type=%u length=%u",
                           map, (unsigned int)type, (unsigned int)length);
                    break;
                }
                if (type == 1U && length == 6U) {
                    uint16_t mapped_partition = read_le16(block + map_offset + consumed + 4U);
                    report("UDF_PARTITION_MAP map=%u type=1 volume_seq=%u partition=%u",
                           map, (unsigned int)read_le16(block + map_offset + consumed + 2U),
                           (unsigned int)mapped_partition);
                    if (mapped_partition == partition_number) {
                        matching_type1_map = 1;
                        mapped_partition_number = mapped_partition;
                        physical_partition_ref = (uint16_t)map;
                    }
                } else if (type == 2U && length == 64U) {
                    const uint8_t *entity_bytes = block + map_offset + consumed + 5U;
                    char entity[24];
                    memcpy(entity, entity_bytes, 23U);
                    entity[23] = '\0';
                    uint16_t volume_sequence = read_le16(
                        block + map_offset + consumed + 36U);
                    uint16_t mapped_partition = read_le16(
                        block + map_offset + consumed + 38U);
                    size_t entity_length = strlen(entity);
                    while (entity_length != 0U &&
                           (entity[entity_length - 1U] == ' ' ||
                            entity[entity_length - 1U] == '\0'))
                        entity[--entity_length] = '\0';
                    report("UDF_PARTITION_MAP map=%u type=2 volume_seq=%u partition=%u entity=%s",
                           map, (unsigned int)volume_sequence,
                           (unsigned int)mapped_partition, entity);
                    if (map == fsd_partition && mapped_partition == partition_number &&
                        strncmp(entity, "*UDF Virtual Partition", 22U) == 0) {
                        matching_virtual_map = 1;
                        mapped_partition_number = mapped_partition;
                    }
                } else {
                    report("UDF_PARTITION_MAP map=%u type=%u length=%u unsupported=yes",
                           map, (unsigned int)type, (unsigned int)length);
                }
                consumed += length;
            }
            have_logical_volume = 1;
        }
    }

    if (!have_partition || !have_logical_volume) {
        report("UDF_SCAN stopped partition_found=%s logical_volume_found=%s",
               have_partition ? "yes" : "no", have_logical_volume ? "yes" : "no");
        return;
    }
    if (!matching_type1_map && !matching_virtual_map) {
        report("UDF_SCAN stopped fsd_partition_ref=%u has_no_supported_map",
               (unsigned int)fsd_partition);
        return;
    }
    if (mapped_partition_number != partition_number || partition_length == 0U ||
        partition_start > g_capacity_last_lba || fsd_lbn >= partition_length) {
        report("UDF_SCAN stopped invalid_partition_or_fsd partition=%u mapped_partition=%u fsd_partition_ref=%u start=%u length=%u fsd_lbn=%u",
               (unsigned int)partition_number, (unsigned int)mapped_partition_number,
               (unsigned int)fsd_partition,
               partition_start, partition_length, fsd_lbn);
        return;
    }

    struct udf_scan_context context;
    memset(&context, 0, sizeof(context));
    context.last_lba = g_capacity_last_lba;
    context.partition_start = partition_start;
    context.partition_length = partition_length;
    context.partition_number = partition_number;
    context.logical_partition_ref = fsd_partition;
    context.physical_partition_ref = physical_partition_ref;
    if (matching_virtual_map &&
        udf_load_virtual_allocation_table(&context, fsd_partition) != 0) {
        report("UDF_SCAN stopped virtual_map_without_valid_vat");
        free(context.vat_storage);
        return;
    }
    int current_scan_result = -1;
    if (udf_read_partition_block(&context, fsd_lbn, block,
                                 "UDF_READ_FILESET_DESCRIPTOR") == 0) {
        current_scan_result = udf_scan_fileset_descriptor(
            &context, block, fsd_lbn, fsd_partition, "current-vat");
    }
    if (current_scan_result != 0 && matching_virtual_map)
        (void)udf_probe_previous_vat(&context, fsd_lbn, fsd_partition);
    free(context.vat_storage);
}

static void probe_udf_track_relative_layout(const struct track_info *tracks,
                                           size_t track_count)
{
    if (!g_capacity_valid)
        return;
    for (size_t i = 0; i < track_count; ++i) {
        const struct track_info *track = &tracks[i];
        if (!track->valid || (track->control & 0x04U) == 0 ||
            track->start_lba == 0U || track->end_lba <= track->start_lba)
            continue;

        uint32_t track_start = track->start_lba;
        report("UDF_TRACK_RELATIVE begin track=%u start_lba=%u end_lba=%u",
               (unsigned int)track->number, track_start, track->end_lba);
        for (uint32_t offset = 16U; offset <= 18U; ++offset) {
            uint64_t absolute_lba = (uint64_t)track_start + offset;
            if (absolute_lba >= track->end_lba || absolute_lba > g_capacity_last_lba)
                break;
            uint8_t block[UDF_LOGICAL_BLOCK_SIZE];
            if (udf_read_block((uint32_t)absolute_lba, block,
                               "UDF_READ_TRACK_RELATIVE_VRS") != 0)
                continue;
            char identifier[3U * 5U + 1U];
            hex_bytes(block + 1U, 5U, identifier, sizeof(identifier));
            report("UDF_TRACK_RELATIVE_VRS track=%u offset=%u absolute_lba=%u type=%#04x identifier_hex=%s",
                   (unsigned int)track->number, offset,
                   (unsigned int)absolute_lba, (unsigned int)block[0], identifier);
        }

        uint64_t anchor_lba64 = (uint64_t)track_start + 256U;
        if (anchor_lba64 >= track->end_lba || anchor_lba64 > g_capacity_last_lba) {
            report("UDF_TRACK_RELATIVE anchor=OUT_OF_TRACK track=%u candidate_lba=%llu",
                   (unsigned int)track->number,
                   (unsigned long long)anchor_lba64);
            continue;
        }
        uint32_t anchor_lba = (uint32_t)anchor_lba64;
        uint8_t block[UDF_LOGICAL_BLOCK_SIZE];
        if (udf_read_block(anchor_lba, block,
                           "UDF_READ_TRACK_RELATIVE_ANCHOR") != 0)
            continue;
        uint16_t anchor_tag = read_le16(block);
        int anchor_valid = anchor_tag == 2U && udf_tag_valid(block, 2U);
        udf_report_tag_crc("track-relative-anchor", block);
        report("UDF_TRACK_RELATIVE_ANCHOR track=%u absolute_lba=%u tag_id=%u checksum=%s tag_location=%u",
               (unsigned int)track->number, anchor_lba,
               (unsigned int)anchor_tag, anchor_valid ? "valid" : "invalid",
               read_le32(block + 12U));
        if (!anchor_valid)
            continue;

        uint32_t vds_bytes = read_le32(block + 16U) & 0x3fffffffU;
        uint32_t vds_start = read_le32(block + 20U);
        uint64_t vds_blocks = ((uint64_t)vds_bytes + UDF_LOGICAL_BLOCK_SIZE - 1U) /
                              UDF_LOGICAL_BLOCK_SIZE;
        report("UDF_TRACK_RELATIVE_VDS track=%u pointer_lba=%u bytes=%u blocks=%llu",
               (unsigned int)track->number, vds_start,
               vds_bytes, (unsigned long long)vds_blocks);
        if (vds_bytes == 0U || vds_blocks == 0U ||
            vds_blocks > UDF_MAX_VDS_BLOCKS ||
            (uint64_t)vds_start + vds_blocks > track->end_lba ||
            (uint64_t)vds_start + vds_blocks >
                (uint64_t)g_capacity_last_lba + 1U) {
            report("UDF_TRACK_RELATIVE_VDS skipped invalid_extent track=%u",
                   (unsigned int)track->number);
            continue;
        }
        for (uint64_t descriptor = 0; descriptor < vds_blocks; ++descriptor) {
            uint32_t lba = vds_start + (uint32_t)descriptor;
            if (udf_read_block(lba, block, "UDF_READ_TRACK_RELATIVE_VDS") != 0)
                break;
            uint16_t tag_id = read_le16(block);
            int tag_valid = udf_tag_valid(block, tag_id);
            udf_report_tag_crc("track-relative-vds", block);
            report("UDF_TRACK_RELATIVE_DESCRIPTOR track=%u lba=%u tag_id=%u checksum=%s tag_location=%u",
                   (unsigned int)track->number, lba, (unsigned int)tag_id,
                   tag_valid ? "valid" : "invalid",
                   read_le32(block + 12U));
            if (!tag_valid)
                continue;
            if (tag_id == 5U) {
                uint32_t partition_start = read_le32(block + 188U);
                uint32_t partition_length = read_le32(block + 192U);
                report("UDF_TRACK_RELATIVE_PARTITION track=%u number=%u relative_start=%u length=%u relative_end=%llu",
                       (unsigned int)track->number,
                       (unsigned int)read_le16(block + 22U), partition_start,
                       partition_length,
                       (unsigned long long)partition_start + partition_length);
            } else if (tag_id == 6U) {
                report("UDF_TRACK_RELATIVE_LOGICAL_VOLUME track=%u block_size=%u fsd_extent_bytes=%u fsd_lbn=%u fsd_partition=%u map_count=%u map_length=%u",
                       (unsigned int)track->number, read_le32(block + 212U),
                       read_le32(block + 248U) & 0x3fffffffU,
                       read_le32(block + 252U),
                       (unsigned int)read_le16(block + 256U),
                       read_le32(block + 268U), read_le32(block + 264U));
            } else if (tag_id == 8U) {
                break;
            }
        }
        probe_udf_volume(anchor_lba);
    }
}

static void read_audio_sample(const struct track_info *track, const char *position,
                              uint32_t sample_lba)
{
    if (track->end_lba <= track->start_lba || sample_lba < track->start_lba ||
        sample_lba >= track->end_lba)
        return;
    uint32_t sectors = track->end_lba - sample_lba;
    if (sectors > AUDIO_SAMPLE_SECTORS)
        sectors = AUDIO_SAMPLE_SECTORS;
    size_t bytes = (size_t)sectors * AUDIO_BYTES_PER_SECTOR;
    uint8_t pcm[AUDIO_SAMPLE_SECTORS * AUDIO_BYTES_PER_SECTOR];
    memset(pcm, 0, bytes);

    uint32_t lba = sample_lba;
    uint8_t cdb[12] = {0xbeU, 0x04U, 0, 0, 0, 0, 0, 0, 0, 0x10U, 0, 0};
    cdb[2] = (uint8_t)(lba >> 24);
    cdb[3] = (uint8_t)(lba >> 16);
    cdb[4] = (uint8_t)(lba >> 8);
    cdb[5] = (uint8_t)lba;
    cdb[8] = (uint8_t)sectors;
    if (issue_scsi("READ_CD_AUDIO_PCM_IN_MEMORY", cdb, sizeof(cdb), pcm, bytes) != 0) {
        report("READ_CD_AUDIO result=FAIL track=%u position=%s lba=%u sectors=%u",
               (unsigned int)track->number, position, lba, sectors);
        return;
    }

    uint64_t nonzero = 0;
    int32_t minimum = 32767;
    int32_t maximum = -32768;
    for (size_t i = 0; i + 1U < bytes; i += 2U) {
        uint16_t raw = (uint16_t)((uint16_t)pcm[i] | ((uint16_t)pcm[i + 1U] << 8));
        int32_t sample = (raw & 0x8000U) != 0 ? (int32_t)raw - 65536 : (int32_t)raw;
        if (sample < minimum)
            minimum = sample;
        if (sample > maximum)
            maximum = sample;
        if (sample != 0)
            ++nonzero;
    }
    report("READ_CD_AUDIO result=PASS track=%u position=%s lba=%u sectors=%u bytes=%u nonzero=%llu min=%ld max=%ld fnv1a32=%08x",
           (unsigned int)track->number, position, lba, sectors, (unsigned int)bytes,
           (unsigned long long)nonzero, (long)minimum, (long)maximum,
           fnv1a32(pcm, bytes));
}

static void sample_audio_track(const struct track_info *track)
{
    if (track->end_lba <= track->start_lba)
        return;
    uint32_t length = track->end_lba - track->start_lba;
    uint32_t tail_lba = track->end_lba - 1U;
    uint32_t middle_lba = track->start_lba + length / 2U;
    read_audio_sample(track, "start", track->start_lba);
    if (middle_lba != track->start_lba && middle_lba != tail_lba)
        read_audio_sample(track, "middle", middle_lba);
    if (tail_lba != track->start_lba && tail_lba != middle_lba)
        read_audio_sample(track, "tail", tail_lba);
}

static int safe_mountpoint(void)
{
    struct statfs base;
    if (statfs("/mnt", &base) != 0) {
        int saved_errno = errno;
        report("statfs base_mount result=FAIL errno=%d (%s)", saved_errno,
               strerror(saved_errno));
        return 0;
    }
    report("statfs base_mount fs=%s from=%s on=%s flags=%#llx",
           base.f_fstypename, base.f_mntfromname, base.f_mntonname,
           (unsigned long long)base.f_flags);
    if (strcmp(base.f_mntonname, "/mnt") != 0 ||
        strcmp(base.f_fstypename, "tmpfs") != 0) {
        report("MOUNT_SKIPPED /mnt is not the expected tmpfs mount");
        return 0;
    }

    static const char *const candidates[] = {
        "/mnt/disc", "/mnt/disc1", "/mnt/dvd", "/mnt/dvd1",
        "/mnt/cd1", "/mnt/optical", "/mnt/optical1", "/mnt/media1"
    };
    for (size_t i = 0; i < sizeof(candidates) / sizeof(candidates[0]); ++i) {
        const char *candidate = candidates[i];
        struct stat directory_info;
        if (lstat(candidate, &directory_info) != 0) {
            int saved_errno = errno;
            report("MOUNT_CANDIDATE path=%s result=unavailable errno=%d",
                   candidate, saved_errno);
            continue;
        }
        if (!S_ISDIR(directory_info.st_mode) || S_ISLNK(directory_info.st_mode)) {
            report("MOUNT_CANDIDATE path=%s result=not_real_directory", candidate);
            continue;
        }

        struct statfs info;
        if (statfs(candidate, &info) != 0) {
            int saved_errno = errno;
            report("MOUNT_CANDIDATE path=%s result=statfs_failed errno=%d",
                   candidate, saved_errno);
            continue;
        }
        if (strcmp(info.f_mntonname, "/mnt") != 0 ||
            strcmp(info.f_fstypename, base.f_fstypename) != 0) {
            report("MOUNT_CANDIDATE path=%s result=already_mounted fs=%s from=%s on=%s",
                   candidate, info.f_fstypename, info.f_mntfromname,
                   info.f_mntonname);
            continue;
        }

        DIR *directory = opendir(candidate);
        if (directory == NULL) {
            int saved_errno = errno;
            report("MOUNT_CANDIDATE path=%s result=opendir_failed errno=%d",
                   candidate, saved_errno);
            continue;
        }
        int empty = 1;
        struct dirent *entry;
        while ((entry = readdir(directory)) != NULL) {
            if (strcmp(entry->d_name, ".") != 0 &&
                strcmp(entry->d_name, "..") != 0) {
                empty = 0;
                break;
            }
        }
        closedir(directory);
        if (!empty) {
            report("MOUNT_CANDIDATE path=%s result=not_empty", candidate);
            continue;
        }

        int length = snprintf(g_mount_path, sizeof(g_mount_path), "%s", candidate);
        if (length <= 0 || (size_t)length >= sizeof(g_mount_path)) {
            g_mount_path[0] = '\0';
            report("MOUNT_CANDIDATE path=%s result=path_too_long", candidate);
            continue;
        }
        report("MOUNTPOINT_CHOSEN path=%s existing=yes empty=yes fs=%s readonly_required=yes",
               g_mount_path, info.f_fstypename);
        return 1;
    }

    report("MOUNT_SKIPPED no pre-existing empty optical mountpoint under /mnt; no directory created");
    return 0;
}

/* Returns -1 for a failed mount, 0 for a verified read-only mount, 1 if a
 * mount succeeded but could not be safely identified (stop trying types). */
static int try_mount_type(const char *filesystem)
{
    struct iovec options[6];
    const char *names[3] = {"fstype", "fspath", "from"};
    const char *values[3] = {filesystem, g_mount_path, DEVICE_PATH};
    for (size_t i = 0; i < 3U; ++i) {
        options[i * 2U].iov_base = (void *)names[i];
        options[i * 2U].iov_len = strlen(names[i]) + 1U;
        options[i * 2U + 1U].iov_base = (void *)values[i];
        options[i * 2U + 1U].iov_len = strlen(values[i]) + 1U;
    }
    report("MOUNT_BEGIN type=%s path=%s source=%s mode=readonly", filesystem,
           g_mount_path, DEVICE_PATH);
    if (nmount(options, 6U, MNT_RDONLY) != 0) {
        int saved_errno = errno;
        report("MOUNT_END type=%s result=FAIL errno=%d (%s)", filesystem,
               saved_errno, strerror(saved_errno));
        return -1;
    }
    report("MOUNT_END type=%s result=PASS", filesystem);
    struct statfs mounted;
    if (statfs(g_mount_path, &mounted) != 0) {
        int saved_errno = errno;
        report("statfs mounted path failed errno=%d", saved_errno);
        return 1;
    }
    report("MOUNT_VERIFY fs=%s from=%s on=%s readonly=%s",
           mounted.f_fstypename, mounted.f_mntfromname, mounted.f_mntonname,
           (mounted.f_flags & MNT_RDONLY) != 0 ? "yes" : "no");
    if (strcmp(mounted.f_mntonname, g_mount_path) != 0 ||
        strcmp(mounted.f_mntfromname, DEVICE_PATH) != 0 ||
        (mounted.f_flags & MNT_RDONLY) == 0) {
        report("MOUNT_VERIFY result=FAIL unexpected source/path/flags; leaving mount untouched");
        return 1;
    }
    return 0;
}

static int read_file_sample(const char *path)
{
    int fd = open(path, O_RDONLY);
    if (fd < 0) {
        int saved_errno = errno;
        report("FILE_OPEN path=%s result=FAIL errno=%d", path, saved_errno);
        return -1;
    }
    uint8_t bytes[READ_BYTES];
    ssize_t count;
    do {
        count = read(fd, bytes, sizeof(bytes));
    } while (count < 0 && errno == EINTR);
    int saved_errno = count < 0 ? errno : 0;
    close(fd);
    if (count < 0) {
        report("FILE_READ path=%s result=FAIL errno=%d", path, saved_errno);
        return -1;
    }
    report("FILE_READ path=%s result=PASS bytes=%u fnv1a32=%08x",
           path, (unsigned int)count, fnv1a32(bytes, (size_t)count));
    g_media_sample_read = 1;
    return 0;
}

static void scan_directory(const char *path, unsigned int depth)
{
    if (depth > MAX_SCAN_DEPTH || g_entries_seen >= MAX_DIR_ENTRIES)
        return;
    DIR *directory = opendir(path);
    if (directory == NULL) {
        int saved_errno = errno;
        report("DIRECTORY_OPEN path=%s result=FAIL errno=%d", path, saved_errno);
        return;
    }
    struct dirent *entry;
    while (g_entries_seen < MAX_DIR_ENTRIES && (entry = readdir(directory)) != NULL) {
        if (strcmp(entry->d_name, ".") == 0 || strcmp(entry->d_name, "..") == 0)
            continue;
        ++g_entries_seen;
        char child[512];
        int length = snprintf(child, sizeof(child), "%s/%.*s", path,
                              (int)MAX_NAME_BYTES, entry->d_name);
        if (length <= 0 || (size_t)length >= sizeof(child)) {
            report("DIRECTORY_ENTRY path overflow");
            continue;
        }
        struct stat info;
        if (lstat(child, &info) != 0) {
            int saved_errno = errno;
            report("DIRECTORY_ENTRY_STAT path=%s result=FAIL errno=%d", child,
                   saved_errno);
            continue;
        }
        if (S_ISREG(info.st_mode)) {
            ++g_files_seen;
            int media_index = media_extension_index(entry->d_name);
            if (media_index >= 0) {
                ++g_media_counts[(size_t)media_index];
                if (g_media_path_logs < MAX_MEDIA_PATH_LOGS) {
                    report("MEDIA_FILE type=%s size=%lld path=%s",
                           g_media_extensions[(size_t)media_index],
                           (long long)info.st_size, child);
                    ++g_media_path_logs;
                }
                if (!g_media_sample_attempted) {
                    g_media_sample_attempted = 1;
                    (void)read_file_sample(child);
                }
            }
        } else if (S_ISDIR(info.st_mode) && depth < MAX_SCAN_DEPTH) {
            scan_directory(child, depth + 1U);
        }
    }
    closedir(directory);
}

static void read_mounted_contents(void)
{
    g_entries_seen = 0;
    g_files_seen = 0;
    g_media_path_logs = 0;
    g_media_sample_attempted = 0;
    g_media_sample_read = 0;
    memset(g_media_counts, 0, sizeof(g_media_counts));
    scan_directory(g_mount_path, 0);
    report("MEDIA_SCAN entries=%u files=%u mp3=%u wma=%u wav=%u m4a=%u mp4a=%u aac=%u flac=%u ogg=%u opus=%u sample_read=%s",
           (unsigned int)g_entries_seen, (unsigned int)g_files_seen,
           (unsigned int)g_media_counts[0], (unsigned int)g_media_counts[1],
           (unsigned int)g_media_counts[2], (unsigned int)g_media_counts[3],
           (unsigned int)g_media_counts[4], (unsigned int)g_media_counts[5],
           (unsigned int)g_media_counts[6], (unsigned int)g_media_counts[7],
           (unsigned int)g_media_counts[8], g_media_sample_read ? "yes" : "no");
}

static void unmount_own_readonly_media(void)
{
    struct statfs info;
    if (statfs(g_mount_path, &info) != 0 ||
        strcmp(info.f_mntonname, g_mount_path) != 0 ||
        strcmp(info.f_mntfromname, DEVICE_PATH) != 0 ||
        (info.f_flags & MNT_RDONLY) == 0) {
        report("UNMOUNT_SKIPPED mount identity changed or is not verified read-only");
        return;
    }
    if (unmount(g_mount_path, 0) != 0) {
        int saved_errno = errno;
        report("UNMOUNT result=FAIL errno=%d (%s); read-only mount remains",
               saved_errno, strerror(saved_errno));
        return;
    }
    report("UNMOUNT result=PASS path=%s", g_mount_path);
}

int main(int argc, char **argv)
{
    (void)argc;
    (void)argv;
    report("START schema=1 epoch=%lld pid=%ld uid=%u euid=%u device=%s",
           (long long)time(NULL), (long)getpid(), (unsigned int)getuid(),
           (unsigned int)geteuid(), DEVICE_PATH);
    report("SAFETY no console files created/modified/deleted; only read CDBs; PCM in RAM; any mount readonly");

    struct stat device_info;
    if (stat(DEVICE_PATH, &device_info) != 0) {
        int saved_errno = errno;
        report("DEVICE_STAT result=FAIL errno=%d (%s)", saved_errno, strerror(saved_errno));
        report("END result=NO_DEVICE");
        return 0;
    }
    int cd_fd = open(DEVICE_PATH, O_RDONLY);
    if (cd_fd < 0) {
        int saved_errno = errno;
        report("DEVICE_OPEN result=FAIL errno=%d (%s)", saved_errno, strerror(saved_errno));
        report("END result=DEVICE_OPEN_FAILED");
        return 0;
    }
    report("DEVICE_OPEN result=PASS mode=readonly");

    g_pass_fd = open_pass_device(cd_fd);
    if (g_pass_fd >= 0) {
        scsi_basics();
        read_disc_information();
        if (profile_is_dvd()) {
            for (uint8_t track_number = 1U; track_number <= 3U; ++track_number)
                read_track_information(track_number);
        }
        read_auxiliary_disc_metadata(1U, "READ_TOC_FORMAT_1_SESSION");
        read_auxiliary_disc_metadata(2U, "READ_TOC_FORMAT_2_FULL_TOC");
        read_auxiliary_disc_metadata(4U, "READ_TOC_FORMAT_4_ATIP");
    }

    struct track_info tracks[MAX_TOC_TRACKS];
    memset(tracks, 0, sizeof(tracks));
    size_t track_count = 0;
    uint32_t leadout = 0;
    int toc_result = read_toc_ioctl(cd_fd, tracks, &track_count, &leadout);
    if (toc_result != 0 && g_pass_fd >= 0) {
        report("TOC_IOCTL unavailable; trying read-only SCSI READ TOC");
        toc_result = read_toc_scsi(tracks, &track_count, &leadout);
    }
    const struct track_info *first_audio = NULL;
    const struct track_info *last_audio = NULL;
    const struct track_info *first_data = NULL;
    size_t audio_track_count = 0;
    size_t data_track_count = 0;
    if (toc_result == 0) {
        finish_track_bounds(tracks, track_count, leadout);
        for (size_t i = 0; i < track_count; ++i) {
            if (!tracks[i].valid || tracks[i].end_lba <= tracks[i].start_lba)
                continue;
            if ((tracks[i].control & 0x04U) == 0) {
                if (first_audio == NULL)
                    first_audio = &tracks[i];
                last_audio = &tracks[i];
                ++audio_track_count;
            } else {
                if (first_data == NULL)
                    first_data = &tracks[i];
                ++data_track_count;
            }
        }
        report("TOC_SUMMARY tracks=%u leadout=%u audio_tracks=%u data_tracks=%u first_audio=%u last_audio=%u first_data=%u",
               (unsigned int)track_count, leadout,
               (unsigned int)audio_track_count, (unsigned int)data_track_count,
               first_audio == NULL ? 0U : (unsigned int)first_audio->number,
               last_audio == NULL ? 0U : (unsigned int)last_audio->number,
               first_data == NULL ? 0U : (unsigned int)first_data->number);
        if (g_pass_fd >= 0) {
            if (first_data != NULL)
                (void)read_data_sector(first_data);
            else
                report("READ10_DATA skipped no data track");
            if (profile_is_dvd())
            {
                probe_dvd_volume_sectors();
                probe_udf_volume(256U);
                probe_udf_track_relative_layout(tracks, track_count);
            }
            if (first_audio != NULL) {
                sample_audio_track(first_audio);
                if (last_audio != first_audio)
                    sample_audio_track(last_audio);
            } else {
                report("READ_CD_AUDIO skipped no audio track");
            }
        }
    } else {
        report("TOC_RESULT unavailable; no LBA reads attempted");
        if (g_pass_fd >= 0 && profile_is_dvd()) {
            probe_dvd_volume_sectors();
            probe_udf_volume(256U);
            probe_udf_track_relative_layout(tracks, track_count);
        }
    }

    int may_have_data_filesystem = first_data != NULL || profile_is_dvd() ||
                                   (toc_result != 0 && g_unit_ready);
    if (may_have_data_filesystem && safe_mountpoint() &&
        (g_unit_ready || toc_result == 0)) {
        static const char *mount_types[] = {"cd9660", "udf2", "udf"};
        int mounted = 0;
        for (size_t i = 0; i < sizeof(mount_types) / sizeof(mount_types[0]); ++i) {
            int mount_result = try_mount_type(mount_types[i]);
            if (mount_result == 0) {
                mounted = 1;
                read_mounted_contents();
                unmount_own_readonly_media();
                break;
            }
            if (mount_result > 0) {
                mounted = 1;
                break;
            }
        }
        if (!mounted)
            report("MOUNT_RESULT no supported filesystem mounted read-only");
    } else if (!may_have_data_filesystem) {
        report("MOUNT_SKIPPED TOC indicates audio-only media or no data profile");
    } else if (!g_unit_ready && toc_result != 0) {
        report("MOUNT_SKIPPED medium not ready and no readable TOC");
    }

    if (g_pass_fd >= 0)
        close(g_pass_fd);
    close(cd_fd);
    report("END result=%s toc=%s profile=%#06x profile_valid=%s audio_tracks=%u data_tracks=%u file_sample_read=%s",
           g_media_sample_read ? "MEDIA_FILE_READ_PASS" : "PROBE_COMPLETE",
           toc_result == 0 ? "available" : "unavailable",
           (unsigned int)g_current_profile, g_profile_valid ? "yes" : "no",
           (unsigned int)audio_track_count, (unsigned int)data_track_count,
           g_media_sample_read ? "yes" : "no");
    return 0;
}
