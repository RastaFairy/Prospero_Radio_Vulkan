// Prospero Radio - standalone CD-DA / CD-ROM diagnostic payload.
// Copyright (C) 2026 BlackBearReloaded
// SPDX-License-Identifier: GPL-3.0-or-later

#include <sys/types.h>
#include <sys/cdio.h>
#include <sys/mount.h>
#include <sys/stat.h>

#include <stdio.h>
#include <cam/cam.h>
#include <cam/cam_ccb.h>
#include <cam/scsi/scsi_all.h>
#include <cam/scsi/scsi_pass.h>

#include <ps5/klog.h>

#include <errno.h>
#include <fcntl.h>
#include <stdarg.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ioctl.h>
#include <time.h>
#include <unistd.h>

#define CDDA_DATA_DIRECTORY "/data/cdda"
#define CDDA_DEVICE "/dev/cd0"
#define DISC_MOUNT_PATH "/mnt/disc"
#define MAX_LOG_BYTES (64U * 1024U)
#define CDDA_BYTES_PER_SECTOR 2352U
#define CDROM_BYTES_PER_SECTOR 2048U
#define SAMPLE_MAX_SECTORS 75U
#define READ_CD_MAX_SECTORS 25U
#define CDROM_LEADOUT_TRACK 0xaaU

struct track_info {
    uint8_t number;
    uint8_t control;
    uint32_t start_lba;
    uint32_t end_lba;
    int valid;
};

static int g_log_fd = -1;
static size_t g_log_bytes;
static int g_log_limit_reported;
static char g_session_base[256];
static char g_log_path[288];
static int g_pass_fd = -1;
static path_id_t g_cam_path_id;
static target_id_t g_cam_target_id;
static lun_id_t g_cam_target_lun;

static int write_all(int fd, const void *data, size_t size)
{
    const unsigned char *bytes = (const unsigned char *)data;
    while (size > 0) {
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

static void report(const char *format, ...)
{
    char message[900];
    char line[920];
    va_list args;
    va_start(args, format);
    int message_size = vsnprintf(message, sizeof(message), format, args);
    va_end(args);
    if (message_size < 0)
        return;
    if ((size_t)message_size >= sizeof(message))
        message_size = (int)sizeof(message) - 1;

    int line_size = snprintf(line, sizeof(line), "[CDDA-PROBE] %s\n", message);
    if (line_size < 0)
        return;
    if ((size_t)line_size >= sizeof(line))
        line_size = (int)sizeof(line) - 1;

    klog_printf("[CDDA-PROBE] %s\n", message);
    if (g_log_fd < 0)
        return;

    size_t bytes = (size_t)line_size;
    if (g_log_bytes > MAX_LOG_BYTES || bytes > MAX_LOG_BYTES - g_log_bytes) {
        if (!g_log_limit_reported) {
            g_log_limit_reported = 1;
            klog_printf("[CDDA-PROBE] persistent log reached %u-byte limit\n",
                        (unsigned int)MAX_LOG_BYTES);
        }
        return;
    }
    if (write_all(g_log_fd, line, bytes) != 0) {
        int saved_errno = errno;
        klog_printf("[CDDA-PROBE] persistent log write failed errno=%d\n", saved_errno);
        close(g_log_fd);
        g_log_fd = -1;
        return;
    }
    g_log_bytes += bytes;
    if (fsync(g_log_fd) != 0) {
        int saved_errno = errno;
        klog_printf("[CDDA-PROBE] persistent log fsync failed errno=%d\n", saved_errno);
        close(g_log_fd);
        g_log_fd = -1;
    }
}

static int create_session_log(void)
{
    struct stat info;
    if (mkdir(CDDA_DATA_DIRECTORY, 0755) != 0 && errno != EEXIST) {
        int saved_errno = errno;
        klog_printf("[CDDA-PROBE] mkdir %s failed errno=%d\n",
                    CDDA_DATA_DIRECTORY, saved_errno);
        return -1;
    }
    if (stat(CDDA_DATA_DIRECTORY, &info) != 0) {
        int saved_errno = errno;
        klog_printf("[CDDA-PROBE] stat %s failed errno=%d\n",
                    CDDA_DATA_DIRECTORY, saved_errno);
        return -1;
    }
    if (!S_ISDIR(info.st_mode)) {
        klog_printf("[CDDA-PROBE] %s exists but is not a directory\n",
                    CDDA_DATA_DIRECTORY);
        return -1;
    }

    time_t now = time(NULL);
    long pid = (long)getpid();
    for (unsigned int suffix = 0; suffix < 100U; ++suffix) {
        int base_size;
        if (suffix == 0U) {
            base_size = snprintf(g_session_base, sizeof(g_session_base),
                                 "%s/probe-%lld-%ld", CDDA_DATA_DIRECTORY,
                                 (long long)now, pid);
        } else {
            base_size = snprintf(g_session_base, sizeof(g_session_base),
                                 "%s/probe-%lld-%ld-%u", CDDA_DATA_DIRECTORY,
                                 (long long)now, pid, suffix);
        }
        if (base_size <= 0 || (size_t)base_size >= sizeof(g_session_base))
            return -1;
        int log_size = snprintf(g_log_path, sizeof(g_log_path), "%s.log", g_session_base);
        if (log_size <= 0 || (size_t)log_size >= sizeof(g_log_path))
            return -1;
        g_log_fd = open(g_log_path, O_WRONLY | O_CREAT | O_EXCL | O_APPEND, 0644);
        if (g_log_fd >= 0) {
            g_log_bytes = 0;
            return 0;
        }
        if (errno != EEXIST) {
            int saved_errno = errno;
            klog_printf("[CDDA-PROBE] create %s failed errno=%d\n",
                        g_log_path, saved_errno);
            return -1;
        }
    }
    klog_printf("[CDDA-PROBE] could not reserve a unique report file\n");
    return -1;
}

static uint32_t read_be32(const uint8_t bytes[4])
{
    return ((uint32_t)bytes[0] << 24) | ((uint32_t)bytes[1] << 16) |
           ((uint32_t)bytes[2] << 8) | (uint32_t)bytes[3];
}

static void write_le16(uint8_t *bytes, uint16_t value)
{
    bytes[0] = (uint8_t)(value & 0xffU);
    bytes[1] = (uint8_t)(value >> 8);
}

static void write_le32(uint8_t *bytes, uint32_t value)
{
    bytes[0] = (uint8_t)(value & 0xffU);
    bytes[1] = (uint8_t)((value >> 8) & 0xffU);
    bytes[2] = (uint8_t)((value >> 16) & 0xffU);
    bytes[3] = (uint8_t)(value >> 24);
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

static void bytes_to_hex(const uint8_t *bytes, size_t size, char *out, size_t capacity)
{
    size_t used = 0;
    if (capacity == 0)
        return;
    out[0] = '\0';
    for (size_t i = 0; i < size; ++i) {
        if (used + 4U >= capacity)
            break;
        int written = snprintf(out + used, capacity - used, "%s%02x",
                               i == 0 ? "" : " ", (unsigned int)bytes[i]);
        if (written <= 0)
            break;
        used += (size_t)written;
    }
}

static void inspect_mount_path(void)
{
    struct statfs info;
    if (statfs(DISC_MOUNT_PATH, &info) != 0) {
        int saved_errno = errno;
        report("statfs path=%s result=FAIL errno=%d (%s)", DISC_MOUNT_PATH,
               saved_errno, strerror(saved_errno));
        return;
    }
    report("statfs path=%s result=OK fs=%s source=%s mount=%s",
           DISC_MOUNT_PATH, info.f_fstypename, info.f_mntfromname, info.f_mntonname);
}

static void inspect_device_node(const char *path)
{
    struct stat info;
    if (stat(path, &info) != 0) {
        int saved_errno = errno;
        report("stat path=%s result=FAIL errno=%d (%s)", path,
               saved_errno, strerror(saved_errno));
        return;
    }
    report("stat path=%s result=OK mode=%#o rdev=%llu uid=%u gid=%u",
           path, (unsigned int)info.st_mode, (unsigned long long)info.st_rdev,
           (unsigned int)info.st_uid, (unsigned int)info.st_gid);
}

static int open_cam_passthrough(int cd_fd)
{
    union ccb ccb;
    memset(&ccb, 0, sizeof(ccb));
    ccb.ccb_h.func_code = XPT_GDEVLIST;
    if (ioctl(cd_fd, CAMGETPASSTHRU, &ccb) != 0) {
        int saved_errno = errno;
        report("CAMGETPASSTHRU path=%s result=FAIL errno=%d (%s)", CDDA_DEVICE,
               saved_errno, strerror(saved_errno));
        return -1;
    }
    if (ccb.cgdl.status == CAM_GDEVLIST_ERROR) {
        report("CAMGETPASSTHRU path=%s result=CAM_GDEVLIST_ERROR", CDDA_DEVICE);
        return -1;
    }

    char periph_name[DEV_IDLEN + 1];
    memcpy(periph_name, ccb.cgdl.periph_name, DEV_IDLEN);
    periph_name[DEV_IDLEN] = '\0';
    char pass_path[96];
    int path_size = snprintf(pass_path, sizeof(pass_path), "/dev/%s%u",
                             periph_name, ccb.cgdl.unit_number);
    if (path_size <= 0 || (size_t)path_size >= sizeof(pass_path)) {
        report("CAMGETPASSTHRU returned invalid peripheral name");
        return -1;
    }
    report("CAMGETPASSTHRU path=%s result=OK pass=%s", CDDA_DEVICE, pass_path);

    int pass_fd = open(pass_path, O_RDWR);
    if (pass_fd < 0) {
        int saved_errno = errno;
        report("open path=%s result=FAIL errno=%d (%s)", pass_path,
               saved_errno, strerror(saved_errno));
        return -1;
    }

    memset(&ccb, 0, sizeof(ccb));
    ccb.ccb_h.func_code = XPT_GDEVLIST;
    if (ioctl(pass_fd, CAMGETPASSTHRU, &ccb) != 0) {
        int saved_errno = errno;
        report("CAMGETPASSTHRU path=%s result=FAIL errno=%d (%s)", pass_path,
               saved_errno, strerror(saved_errno));
        close(pass_fd);
        return -1;
    }
    if (ccb.cgdl.status == CAM_GDEVLIST_ERROR) {
        report("CAMGETPASSTHRU path=%s result=CAM_GDEVLIST_ERROR", pass_path);
        close(pass_fd);
        return -1;
    }

    g_cam_path_id = ccb.ccb_h.path_id;
    g_cam_target_id = ccb.ccb_h.target_id;
    g_cam_target_lun = ccb.ccb_h.target_lun;
    report("CAM target pass=%s tuple=%u:%u:%llu", pass_path,
           (unsigned int)g_cam_path_id, (unsigned int)g_cam_target_id,
           (unsigned long long)g_cam_target_lun);
    return pass_fd;
}

static int issue_scsi(const char *name, const uint8_t *cdb, uint8_t cdb_size,
                      uint8_t *data, size_t data_size)
{
    if (g_pass_fd < 0 || cdb_size == 0 || cdb_size > CAM_MAX_CDBLEN ||
        data_size > UINT32_MAX) {
        report("SCSI_BEGIN name=%s rejected invalid arguments", name);
        return -1;
    }

    char cdb_text[96];
    bytes_to_hex(cdb, cdb_size, cdb_text, sizeof(cdb_text));
    report("SCSI_BEGIN name=%s cdb=%s dxfer=%u", name, cdb_text,
           (unsigned int)data_size);

    union ccb ccb;
    memset(&ccb, 0, sizeof(ccb));
    ccb.ccb_h.path_id = g_cam_path_id;
    ccb.ccb_h.target_id = g_cam_target_id;
    ccb.ccb_h.target_lun = g_cam_target_lun;
    ccb.ccb_h.flags = (data_size == 0) ? CAM_DIR_NONE : CAM_DIR_IN;
    ccb.ccb_h.flags |= CAM_DEV_QFRZDIS;
    memcpy(ccb.csio.cdb_io.cdb_bytes, cdb, cdb_size);
    cam_fill_csio(&ccb.csio, 0, NULL, ccb.ccb_h.flags, CAM_TAG_ACTION_NONE,
                  data, (uint32_t)data_size,
                  (uint8_t)sizeof(ccb.csio.sense_data), cdb_size, 30000U);
    ccb.ccb_h.path_id = g_cam_path_id;
    ccb.ccb_h.target_id = g_cam_target_id;
    ccb.ccb_h.target_lun = g_cam_target_lun;

    int ioctl_result = ioctl(g_pass_fd, CAMIOCOMMAND, &ccb);
    int saved_errno = (ioctl_result == 0) ? 0 : errno;
    uint32_t cam_status = ccb.ccb_h.status & CAM_STATUS_MASK;
    uint8_t sense_bytes[32];
    size_t sense_size = ccb.csio.sense_len;
    if (sense_size > sizeof(sense_bytes))
        sense_size = sizeof(sense_bytes);
    memcpy(sense_bytes, &ccb.csio.sense_data, sense_size);
    char sense_text[3U * sizeof(sense_bytes) + 1U];
    bytes_to_hex(sense_bytes, sense_size, sense_text, sizeof(sense_text));
    report("SCSI_END name=%s ioctl=%s errno=%d cam_status=%#x scsi_status=%#x resid=%u sense_len=%u sense=%s",
           name, ioctl_result == 0 ? "OK" : "FAIL", saved_errno, cam_status,
           (unsigned int)ccb.csio.scsi_status, (unsigned int)ccb.csio.resid,
           (unsigned int)ccb.csio.sense_len, sense_size == 0 ? "none" : sense_text);

    if (ioctl_result != 0 || cam_status != CAM_REQ_CMP ||
        ccb.csio.scsi_status != 0 || ccb.csio.resid != 0)
        return -1;
    return 0;
}

static void probe_scsi_identity(void)
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
    (void)issue_scsi("TEST_UNIT_READY", ready_cdb, sizeof(ready_cdb), NULL, 0);

    uint8_t config_cdb[10] = {0x46U, 0x01U, 0, 0, 0, 0, 0, 0, 8U, 0};
    uint8_t config[8];
    memset(config, 0, sizeof(config));
    if (issue_scsi("GET_CONFIGURATION_CURRENT", config_cdb,
                   sizeof(config_cdb), config, sizeof(config)) == 0) {
        uint16_t profile = (uint16_t)(((uint16_t)config[6] << 8) | config[7]);
        report("GET_CONFIGURATION profile=%#06x response_length=%u",
               (unsigned int)profile,
               (unsigned int)(((uint16_t)config[0] << 8) | config[1]));
    }
}

static int read_toc(int cd_fd, struct track_info tracks[100], size_t *track_count,
                    uint32_t *leadout_lba)
{
    struct ioc_toc_header header;
    memset(&header, 0, sizeof(header));
    if (ioctl(cd_fd, CDIOREADTOCHEADER, &header) != 0) {
        int saved_errno = errno;
        report("CDIOREADTOCHEADER result=FAIL errno=%d (%s)",
               saved_errno, strerror(saved_errno));
        *track_count = 0;
        return -1;
    }
    report("CDIOREADTOCHEADER result=OK first_track=%u last_track=%u length=%u",
           (unsigned int)header.starting_track, (unsigned int)header.ending_track,
           (unsigned int)header.len);
    if (header.starting_track == 0 || header.ending_track < header.starting_track ||
        (unsigned int)(header.ending_track - header.starting_track) >= 100U) {
        report("TOC header rejected invalid track range");
        *track_count = 0;
        return -1;
    }

    size_t count = (size_t)(header.ending_track - header.starting_track) + 1U;
    memset(tracks, 0, sizeof(struct track_info) * 100U);
    for (size_t i = 0; i < count; ++i) {
        uint8_t track = (uint8_t)(header.starting_track + i);
        struct ioc_read_toc_single_entry entry;
        memset(&entry, 0, sizeof(entry));
        entry.address_format = CD_LBA_FORMAT;
        entry.track = track;
        if (ioctl(cd_fd, CDIOREADTOCENTRY, &entry) != 0) {
            int saved_errno = errno;
            report("CDIOREADTOCENTRY track=%u result=FAIL errno=%d (%s)",
                   (unsigned int)track, saved_errno, strerror(saved_errno));
            continue;
        }
        tracks[i].number = entry.entry.track;
        tracks[i].control = (uint8_t)entry.entry.control;
        tracks[i].start_lba = read_be32(entry.entry.addr.addr);
        tracks[i].valid = 1;
        report("TOC_TRACK track=%u control=%#x type=%s lba=%u",
               (unsigned int)tracks[i].number, (unsigned int)tracks[i].control,
               (tracks[i].control & 0x04U) ? "data" : "audio", tracks[i].start_lba);
    }

    struct ioc_read_toc_single_entry leadout;
    memset(&leadout, 0, sizeof(leadout));
    leadout.address_format = CD_LBA_FORMAT;
    leadout.track = CDROM_LEADOUT_TRACK;
    if (ioctl(cd_fd, CDIOREADTOCENTRY, &leadout) != 0) {
        int saved_errno = errno;
        report("CDIOREADTOCENTRY leadout result=FAIL errno=%d (%s)",
               saved_errno, strerror(saved_errno));
        *track_count = count;
        return -1;
    }
    *leadout_lba = read_be32(leadout.entry.addr.addr);
    report("TOC_LEADOUT lba=%u", *leadout_lba);

    for (size_t i = 0; i < count; ++i) {
        if (!tracks[i].valid)
            continue;
        tracks[i].end_lba = (i + 1U < count && tracks[i + 1U].valid)
                                ? tracks[i + 1U].start_lba
                                : *leadout_lba;
        if (tracks[i].end_lba <= tracks[i].start_lba)
            report("TOC_TRACK track=%u has invalid end_lba=%u",
                   (unsigned int)tracks[i].number, tracks[i].end_lba);
    }
    *track_count = count;
    return 0;
}

static int read_data_sector(const struct track_info *track)
{
    if (track->end_lba <= track->start_lba || track->start_lba > UINT32_MAX - 16U ||
        track->start_lba + 16U >= track->end_lba) {
        report("READ10 data-sector probe skipped track=%u invalid bounds",
               (unsigned int)track->number);
        return -1;
    }
    uint32_t lba = track->start_lba + 16U;
    uint8_t cdb[10] = {0x28U, 0, 0, 0, 0, 0, 0, 0, 1U, 0};
    cdb[2] = (uint8_t)(lba >> 24);
    cdb[3] = (uint8_t)(lba >> 16);
    cdb[4] = (uint8_t)(lba >> 8);
    cdb[5] = (uint8_t)lba;
    uint8_t sector[CDROM_BYTES_PER_SECTOR];
    memset(sector, 0, sizeof(sector));
    if (issue_scsi("READ10_DATA_LBA_TRACK_PLUS_16", cdb, sizeof(cdb),
                   sector, sizeof(sector)) != 0) {
        report("READ10_DATA result=FAIL track=%u lba=%u block_bytes=%u",
               (unsigned int)track->number, lba, (unsigned int)sizeof(sector));
        return -1;
    }
    report("READ10_DATA result=PASS track=%u lba=%u block_bytes=%u fnv1a32=%08x",
           (unsigned int)track->number, lba, (unsigned int)sizeof(sector),
           fnv1a32(sector, sizeof(sector)));
    return 0;
}

static int write_wav_sample(const char *path, const uint8_t *pcm, uint32_t pcm_bytes)
{
    uint8_t header[44];
    memset(header, 0, sizeof(header));
    memcpy(header, "RIFF", 4);
    write_le32(header + 4, 36U + pcm_bytes);
    memcpy(header + 8, "WAVEfmt ", 8);
    write_le32(header + 16, 16U);
    write_le16(header + 20, 1U);
    write_le16(header + 22, 2U);
    write_le32(header + 24, 44100U);
    write_le32(header + 28, 176400U);
    write_le16(header + 32, 4U);
    write_le16(header + 34, 16U);
    memcpy(header + 36, "data", 4);
    write_le32(header + 40, pcm_bytes);

    int fd = open(path, O_WRONLY | O_CREAT | O_EXCL, 0644);
    if (fd < 0) {
        int saved_errno = errno;
        report("WAV create path=%s result=FAIL errno=%d (%s)",
               path, saved_errno, strerror(saved_errno));
        return -1;
    }
    int ok = write_all(fd, header, sizeof(header)) == 0 &&
             write_all(fd, pcm, pcm_bytes) == 0 && fsync(fd) == 0;
    int saved_errno = errno;
    if (close(fd) != 0 && ok) {
        ok = 0;
        saved_errno = errno;
    }
    if (!ok) {
        report("WAV write path=%s result=FAIL errno=%d (%s)",
               path, saved_errno, strerror(saved_errno));
        (void)unlink(path);
        return -1;
    }
    return 0;
}

static void report_pcm_stats(const uint8_t *pcm, size_t size)
{
    int32_t minimum = 32767;
    int32_t maximum = -32768;
    uint64_t nonzero = 0;
    size_t samples = size / 2U;
    for (size_t i = 0; i + 1U < size; i += 2U) {
        uint16_t raw = (uint16_t)((uint16_t)pcm[i] | ((uint16_t)pcm[i + 1U] << 8));
        int32_t sample = (raw & 0x8000U) ? (int32_t)raw - 65536 : (int32_t)raw;
        if (sample < minimum)
            minimum = sample;
        if (sample > maximum)
            maximum = sample;
        if (sample != 0)
            ++nonzero;
    }
    report("CDDA_PCM bytes=%u samples=%llu nonzero=%llu min=%ld max=%ld fnv1a32=%08x",
           (unsigned int)size, (unsigned long long)samples,
           (unsigned long long)nonzero, (long)minimum, (long)maximum,
           fnv1a32(pcm, size));
}

static int read_audio_sample(const struct track_info *track)
{
    if (track->end_lba <= track->start_lba) {
        report("READ_CD CDDA sample skipped track=%u invalid bounds",
               (unsigned int)track->number);
        return -1;
    }
    uint32_t available = track->end_lba - track->start_lba;
    uint32_t sectors = available < SAMPLE_MAX_SECTORS ? available : SAMPLE_MAX_SECTORS;
    if (sectors == 0) {
        report("READ_CD CDDA sample skipped track=%u no sectors", (unsigned int)track->number);
        return -1;
    }
    size_t pcm_size = (size_t)sectors * CDDA_BYTES_PER_SECTOR;
    uint8_t *pcm = (uint8_t *)malloc(pcm_size);
    if (pcm == NULL) {
        report("READ_CD CDDA sample allocation failed bytes=%u", (unsigned int)pcm_size);
        return -1;
    }
    memset(pcm, 0, pcm_size);

    uint32_t completed = 0;
    while (completed < sectors) {
        uint32_t batch = sectors - completed;
        if (batch > READ_CD_MAX_SECTORS)
            batch = READ_CD_MAX_SECTORS;
        uint32_t lba = track->start_lba + completed;
        // READ CD: expected sector type 1 (CD-DA), user-data bytes only (2352).
        uint8_t cdb[12] = {0xbeU, 0x04U, 0, 0, 0, 0, 0, 0, 0, 0x10U, 0, 0};
        cdb[2] = (uint8_t)(lba >> 24);
        cdb[3] = (uint8_t)(lba >> 16);
        cdb[4] = (uint8_t)(lba >> 8);
        cdb[5] = (uint8_t)lba;
        cdb[6] = (uint8_t)(batch >> 16);
        cdb[7] = (uint8_t)(batch >> 8);
        cdb[8] = (uint8_t)batch;
        size_t batch_bytes = (size_t)batch * CDDA_BYTES_PER_SECTOR;
        char command_name[64];
        (void)snprintf(command_name, sizeof(command_name), "READ_CD_CDDA_TRACK_%02u",
                       (unsigned int)track->number);
        if (issue_scsi(command_name, cdb, sizeof(cdb),
                       pcm + (size_t)completed * CDDA_BYTES_PER_SECTOR,
                       batch_bytes) != 0) {
            report("READ_CD CDDA result=FAIL track=%u lba=%u sectors=%u",
                   (unsigned int)track->number, lba, batch);
            free(pcm);
            return -1;
        }
        completed += batch;
    }

    char wav_path[288];
    int path_size = snprintf(wav_path, sizeof(wav_path), "%s.wav", g_session_base);
    if (path_size <= 0 || (size_t)path_size >= sizeof(wav_path)) {
        report("WAV sample path overflow");
        free(pcm);
        return -1;
    }
    report("WAV_BEGIN path=%s track=%u lba=%u sectors=%u", wav_path,
           (unsigned int)track->number, track->start_lba, sectors);
    int wav_result = write_wav_sample(wav_path, pcm, (uint32_t)pcm_size);
    if (wav_result == 0) {
        report_pcm_stats(pcm, pcm_size);
        report("WAV_END result=PASS path=%s bytes=%u duration_ms=%u",
               wav_path, (unsigned int)(44U + pcm_size), sectors * 1000U / 75U);
    }
    free(pcm);
    return wav_result;
}

int main(int argc, char **argv)
{
    (void)argc;
    (void)argv;
    if (create_session_log() != 0)
        return 2;
    report("START schema=1 epoch=%lld pid=%ld uid=%u euid=%u device=%s",
           (long long)time(NULL), (long)getpid(), (unsigned int)getuid(),
           (unsigned int)geteuid(), CDDA_DEVICE);
    report("NOTE CD-DA is probed as TOC plus raw PCM; no filesystem mount is attempted");
    report("NOTE missing END marker means the run stopped early; compare PS5 klog separately");

    inspect_mount_path();
    inspect_device_node(CDDA_DEVICE);
    int cd_fd = open(CDDA_DEVICE, O_RDONLY);
    if (cd_fd < 0) {
        int saved_errno = errno;
        report("open path=%s result=FAIL errno=%d (%s)",
               CDDA_DEVICE, saved_errno, strerror(saved_errno));
        report("END result=CD_DEVICE_OPEN_FAILED");
        close(g_log_fd);
        g_log_fd = -1;
        return 0;
    }
    report("open path=%s result=OK fd=%d", CDDA_DEVICE, cd_fd);

    g_pass_fd = open_cam_passthrough(cd_fd);
    if (g_pass_fd >= 0)
        probe_scsi_identity();
    else
        report("SCSI probe unavailable; CDIO TOC ioctl will still be attempted");

    struct track_info tracks[100];
    memset(tracks, 0, sizeof(tracks));
    size_t track_count = 0;
    uint32_t leadout_lba = 0;
    int toc_result = read_toc(cd_fd, tracks, &track_count, &leadout_lba);
    if (toc_result == 0) {
        struct track_info *first_audio = NULL;
        struct track_info *first_data = NULL;
        for (size_t i = 0; i < track_count; ++i) {
            if (!tracks[i].valid || tracks[i].end_lba <= tracks[i].start_lba)
                continue;
            if ((tracks[i].control & 0x04U) == 0 && first_audio == NULL)
                first_audio = &tracks[i];
            if ((tracks[i].control & 0x04U) != 0 && first_data == NULL)
                first_data = &tracks[i];
        }
        report("TOC_SUMMARY tracks=%u leadout_lba=%u audio_track=%u data_track=%u",
               (unsigned int)track_count, leadout_lba,
               first_audio == NULL ? 0U : (unsigned int)first_audio->number,
               first_data == NULL ? 0U : (unsigned int)first_data->number);
        if (g_pass_fd >= 0) {
            if (first_data != NULL)
                (void)read_data_sector(first_data);
            else
                report("READ10_DATA skipped: TOC contains no data track; use a known Mode-1 data CD-ROM to test CD-ROM reads");
            if (first_audio != NULL)
                (void)read_audio_sample(first_audio);
            else
                report("READ_CD CDDA skipped: TOC contains no audio track");
        }
    } else {
        report("TOC unavailable; no sector reads will be issued without track type and bounds");
    }

    if (g_pass_fd >= 0) {
        close(g_pass_fd);
        g_pass_fd = -1;
    }
    close(cd_fd);
    report("END result=PROBE_COMPLETE toc=%s", toc_result == 0 ? "available" : "unavailable");
    if (g_log_fd >= 0) {
        close(g_log_fd);
        g_log_fd = -1;
    }
    return 0;
}
