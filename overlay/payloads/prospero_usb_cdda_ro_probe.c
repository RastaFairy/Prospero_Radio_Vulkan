// Prospero Radio - read-only USB optical CD-DA diagnostic payload.
// Copyright (C) 2026 BlackBearReloaded
// SPDX-License-Identifier: GPL-3.0-or-later

#include <sys/types.h>
#include <sys/cdio.h>

#include <stdio.h>
#include <cam/cam.h>
#include <cam/cam_ccb.h>
#include <cam/scsi/scsi_all.h>
#include <cam/scsi/scsi_pass.h>
#include <ps5/klog.h>

#include <errno.h>
#include <fcntl.h>
#include <stdint.h>
#include <stdarg.h>
#include <string.h>
#include <sys/ioctl.h>
#include <time.h>
#include <unistd.h>

#define CD_DEVICE_PATH "/dev/cd1"
#define PASS_DEVICE_PATH "/dev/pass1"
#define MAX_TRACKS 99U
#define CDDA_BYTES_PER_SECTOR 2352U
#define SAMPLE_SECTORS 75U
#define READ_CD_BATCH_SECTORS 20U
#define CDROM_LEADOUT_TRACK 0xaaU

struct track_info {
    uint8_t number;
    uint8_t control;
    uint32_t start_lba;
    uint32_t end_lba;
    int valid;
};

static int g_pass_fd = -1;
static path_id_t g_path_id;
static target_id_t g_target_id;
static lun_id_t g_target_lun;
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
    klog_printf("[USB-CDDA-RO] %s\n", message);
}

static uint32_t read_be32(const uint8_t *bytes)
{
    return ((uint32_t)bytes[0] << 24) | ((uint32_t)bytes[1] << 16) |
           ((uint32_t)bytes[2] << 8) | (uint32_t)bytes[3];
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

static int issue_read_command(const char *name, const uint8_t *cdb,
                              uint8_t cdb_length, uint8_t *data,
                              size_t data_length)
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
        report("SCSI_BLOCKED opcode=%#x (not a read command)", (unsigned int)cdb[0]);
        return -1;
    }

    char cdb_text[96];
    hex_bytes(cdb, cdb_length, cdb_text, sizeof(cdb_text));
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
        report("SCSI_SENSE name=%s key=%#x asc=%#x ascq=%#x", name,
               (unsigned int)(sense[2] & 0x0fU), (unsigned int)sense[12],
               (unsigned int)sense[13]);
    } else if (sense_size >= 4U && (sense[0] & 0x7eU) == 0x72U) {
        report("SCSI_SENSE name=%s key=%#x asc=%#x ascq=%#x", name,
               (unsigned int)(sense[1] & 0x0fU), (unsigned int)sense[2],
               (unsigned int)sense[3]);
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

static int open_pass(void)
{
    /* PS5 rejects O_RDONLY on CAM pass devices; only the read-command
     * allowlist in issue_read_command() can reach CAMIOCOMMAND. */
    int fd = open(PASS_DEVICE_PATH, O_RDWR);
    if (fd < 0) {
        int saved_errno = errno;
        report("OPEN_PASS result=FAIL errno=%d (%s)", saved_errno,
               strerror(saved_errno));
        return -1;
    }
    union ccb ccb;
    memset(&ccb, 0, sizeof(ccb));
    ccb.ccb_h.func_code = XPT_GDEVLIST;
    if (ioctl(fd, CAMGETPASSTHRU, &ccb) != 0 ||
        ccb.cgdl.status == CAM_GDEVLIST_ERROR) {
        int saved_errno = errno;
        report("CAMGETPASSTHRU result=FAIL errno=%d", saved_errno);
        close(fd);
        return -1;
    }
    g_path_id = ccb.ccb_h.path_id;
    g_target_id = ccb.ccb_h.target_id;
    g_target_lun = ccb.ccb_h.target_lun;
    g_pass_fd = fd;
    report("OPEN_PASS result=PASS path=%s tuple=%u:%u:%llu", PASS_DEVICE_PATH,
           (unsigned int)g_path_id, (unsigned int)g_target_id,
           (unsigned long long)g_target_lun);
    return 0;
}

static void probe_cd_device(void)
{
    int fd = open(CD_DEVICE_PATH, O_RDONLY);
    if (fd < 0) {
        int saved_errno = errno;
        report("OPEN_CD result=FAIL path=%s errno=%d", CD_DEVICE_PATH, saved_errno);
        return;
    }
    struct ioc_toc_header header;
    memset(&header, 0, sizeof(header));
    if (ioctl(fd, CDIOREADTOCHEADER, &header) == 0) {
        report("CDIOREADTOCHEADER result=PASS first=%u last=%u length=%u",
               (unsigned int)header.starting_track,
               (unsigned int)header.ending_track, (unsigned int)header.len);
    } else {
        int saved_errno = errno;
        report("CDIOREADTOCHEADER result=FAIL errno=%d (%s)", saved_errno,
               strerror(saved_errno));
    }
    close(fd);
}

static int read_toc(struct track_info *tracks, size_t *track_count,
                    uint32_t *leadout_lba)
{
    uint8_t cdb[10] = {0x43U, 0, 0, 0, 0, 0, 0, 0, 0, 0};
    uint8_t data[804];
    memset(data, 0, sizeof(data));
    cdb[7] = (uint8_t)(sizeof(data) >> 8);
    cdb[8] = (uint8_t)sizeof(data);
    if (issue_read_command("READ_TOC_FORMAT_0", cdb, sizeof(cdb),
                           data, sizeof(data)) != 0)
        return -1;
    uint16_t response_length = (uint16_t)(((uint16_t)data[0] << 8) | data[1]);
    uint8_t first = data[2];
    uint8_t last = data[3];
    if ((size_t)response_length + 2U > g_last_data_transfer_length) {
        report("TOC_SHORT_RESPONSE declared=%u transferred=%u",
               (unsigned int)response_length + 2U,
               (unsigned int)g_last_data_transfer_length);
        return -1;
    }
    if (response_length < 4U || first == 0 || last < first ||
        (unsigned int)(last - first) >= MAX_TRACKS) {
        report("TOC_INVALID length=%u first=%u last=%u",
               (unsigned int)response_length, (unsigned int)first,
               (unsigned int)last);
        return -1;
    }
    memset(tracks, 0, sizeof(*tracks) * MAX_TRACKS);
    size_t count = (size_t)(last - first) + 1U;
    size_t descriptors = ((size_t)response_length + 2U) / 8U;
    if (descriptors > (sizeof(data) - 4U) / 8U)
        descriptors = (sizeof(data) - 4U) / 8U;
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
        report("TOC_INVALID no leadout descriptor");
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
        report("TOC_TRACK track=%u control=%#x type=%s start=%u end=%u",
               (unsigned int)tracks[i].number, (unsigned int)tracks[i].control,
               (tracks[i].control & 0x04U) != 0 ? "data" : "audio",
               tracks[i].start_lba, tracks[i].end_lba);
    }
    report("TOC_SUMMARY tracks=%u leadout=%u", (unsigned int)count, *leadout_lba);
    return 0;
}

static void read_audio_sample(const struct track_info *track)
{
    if (track->end_lba <= track->start_lba)
        return;
    uint32_t sectors = track->end_lba - track->start_lba;
    if (sectors > SAMPLE_SECTORS)
        sectors = SAMPLE_SECTORS;
    uint32_t sample_start_lba = track->start_lba;
    size_t bytes = (size_t)sectors * CDDA_BYTES_PER_SECTOR;
    uint8_t pcm[SAMPLE_SECTORS * CDDA_BYTES_PER_SECTOR];
    memset(pcm, 0, bytes);
    uint32_t completed = 0;
    while (completed < sectors) {
        uint32_t batch = sectors - completed;
        if (batch > READ_CD_BATCH_SECTORS)
            batch = READ_CD_BATCH_SECTORS;
        uint32_t lba = track->start_lba + completed;
        uint8_t cdb[12] = {0xbeU, 0x04U, 0, 0, 0, 0, 0, 0, 0, 0x10U, 0, 0};
        cdb[2] = (uint8_t)(lba >> 24);
        cdb[3] = (uint8_t)(lba >> 16);
        cdb[4] = (uint8_t)(lba >> 8);
        cdb[5] = (uint8_t)lba;
        cdb[6] = (uint8_t)(batch >> 16);
        cdb[7] = (uint8_t)(batch >> 8);
        cdb[8] = (uint8_t)batch;
        size_t batch_bytes = (size_t)batch * CDDA_BYTES_PER_SECTOR;
        if (issue_read_command("READ_CD_AUDIO_PCM_TO_RAM", cdb, sizeof(cdb),
                               pcm + (size_t)completed * CDDA_BYTES_PER_SECTOR,
                               batch_bytes) != 0) {
            report("READ_CD_AUDIO_BATCH result=FAIL track=%u lba=%u sectors=%u",
                   (unsigned int)track->number, lba, batch);
            return;
        }
        report("READ_CD_AUDIO_BATCH result=PASS lba=%u sectors=%u bytes=%u",
               lba, batch, (unsigned int)batch_bytes);
        completed += batch;
    }

    uint64_t nonzero_le = 0;
    uint64_t nonzero_be = 0;
    int32_t min_le = 32767;
    int32_t max_le = -32768;
    int32_t min_be = 32767;
    int32_t max_be = -32768;
    for (size_t i = 0; i + 1U < bytes; i += 2U) {
        uint16_t raw_le = (uint16_t)((uint16_t)pcm[i] | ((uint16_t)pcm[i + 1U] << 8));
        uint16_t raw_be = (uint16_t)(((uint16_t)pcm[i] << 8) | (uint16_t)pcm[i + 1U]);
        int32_t sample_le = (raw_le & 0x8000U) != 0 ? (int32_t)raw_le - 65536 : (int32_t)raw_le;
        int32_t sample_be = (raw_be & 0x8000U) != 0 ? (int32_t)raw_be - 65536 : (int32_t)raw_be;
        if (sample_le < min_le) min_le = sample_le;
        if (sample_le > max_le) max_le = sample_le;
        if (sample_be < min_be) min_be = sample_be;
        if (sample_be > max_be) max_be = sample_be;
        if (sample_le != 0) ++nonzero_le;
        if (sample_be != 0) ++nonzero_be;
    }
    report("READ_CD_AUDIO result=PASS track=%u lba=%u sectors=%u bytes=%u fnv1a32=%08x",
           (unsigned int)track->number, sample_start_lba, sectors, (unsigned int)bytes,
           fnv1a32(pcm, bytes));
    report("PCM_STATS nonzero_le=%llu min_le=%ld max_le=%ld nonzero_be=%llu min_be=%ld max_be=%ld",
           (unsigned long long)nonzero_le, (long)min_le, (long)max_le,
           (unsigned long long)nonzero_be, (long)min_be, (long)max_be);
}

int main(int argc, char **argv)
{
    (void)argc;
    (void)argv;
    report("START schema=1 epoch=%lld pid=%ld cd=%s pass=%s",
           (long long)time(NULL), (long)getpid(), CD_DEVICE_PATH, PASS_DEVICE_PATH);
    report("SAFETY no console files written; SCSI CDB allowlist is read-only; PCM stays in RAM");
    probe_cd_device();
    if (open_pass() != 0) {
        report("END result=PASS_DEVICE_UNAVAILABLE");
        return 0;
    }

    uint8_t inquiry_cdb[6] = {0x12U, 0, 0, 0, 96U, 0};
    uint8_t inquiry[96];
    memset(inquiry, 0, sizeof(inquiry));
    if (issue_read_command("INQUIRY", inquiry_cdb, sizeof(inquiry_cdb),
                           inquiry, sizeof(inquiry)) == 0) {
        report("INQUIRY vendor='%.*s' product='%.*s' revision='%.*s'",
               8, (const char *)&inquiry[8], 16, (const char *)&inquiry[16],
               4, (const char *)&inquiry[32]);
    }
    uint8_t ready_cdb[6] = {0};
    int ready = issue_read_command("TEST_UNIT_READY", ready_cdb,
                                   sizeof(ready_cdb), NULL, 0) == 0;
    report("MEDIA_READY result=%s", ready ? "yes" : "no");
    uint8_t config_cdb[10] = {0x46U, 0x01U, 0, 0, 0, 0, 0, 0, 8U, 0};
    uint8_t config[8];
    memset(config, 0, sizeof(config));
    if (issue_read_command("GET_CONFIGURATION", config_cdb, sizeof(config_cdb),
                           config, sizeof(config)) == 0) {
        uint16_t profile = (uint16_t)(((uint16_t)config[6] << 8) | config[7]);
        report("GET_CONFIGURATION profile=%#06x response_length=%u",
               (unsigned int)profile,
               (unsigned int)(((uint16_t)config[0] << 8) | config[1]));
    }

    struct track_info tracks[MAX_TRACKS];
    memset(tracks, 0, sizeof(tracks));
    size_t track_count = 0;
    uint32_t leadout = 0;
    int toc_result = read_toc(tracks, &track_count, &leadout);
    if (toc_result == 0) {
        int audio_found = 0;
        for (size_t i = 0; i < track_count; ++i) {
            if (tracks[i].valid && (tracks[i].control & 0x04U) == 0) {
                audio_found = 1;
                read_audio_sample(&tracks[i]);
                break;
            }
        }
        if (!audio_found)
            report("READ_CD_AUDIO skipped: TOC contains no audio track");
    } else {
        report("TOC unavailable; no READ CD sector request issued");
    }
    close(g_pass_fd);
    g_pass_fd = -1;
    report("END result=PROBE_COMPLETE toc=%s", toc_result == 0 ? "available" : "unavailable");
    return 0;
}
