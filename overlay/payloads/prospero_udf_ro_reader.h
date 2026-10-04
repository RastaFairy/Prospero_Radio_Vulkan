/* Bounded read-only UDF reader used by the optical-disc bridge. */
/* Copyright (C) 2026 BlackBearReloaded */
/* SPDX-License-Identifier: GPL-3.0-or-later */

#ifndef PROSPERO_UDF_RO_READER_H
#define PROSPERO_UDF_RO_READER_H

#include <stdint.h>
#include <stddef.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <stdio.h>

#include "../include/radio_disc_protocol.h"

#define RADIO_UDF_BLOCK_BYTES 2048U
#define RADIO_UDF_MAX_VDS_BLOCKS 128U
#define RADIO_UDF_MAX_VAT_BYTES (8U * 1024U * 1024U)
#define RADIO_UDF_MAX_DIRECTORY_BYTES (256U * 1024U)
#define RADIO_UDF_MAX_DIRECTORY_DEPTH 8U
#define RADIO_UDF_MAX_DIRECTORIES 128U
#define RADIO_UDF_MAX_ENTRIES 512U
#define RADIO_UDF_MAX_FILE_EXTENTS 128U
#define RADIO_UDF_PATH_BYTES 512U
#define RADIO_UDF_NAME_BYTES 256U
#define RADIO_UDF_READ_BATCH_BLOCKS 20U

typedef int (*radio_udf_read_blocks_fn)(void *opaque, uint32_t lba,
                                        uint16_t blocks, uint8_t *buffer);

struct radio_udf_ro_extent {
    uint32_t lbn;
    uint32_t bytes;
    uint16_t partition_ref;
};

struct radio_udf_ro_file {
    char path[RADIO_UDF_PATH_BYTES];
    char name[RADIO_DISC_NAME_BYTES + 1U];
    uint64_t size;
    uint32_t icb_lbn;
    uint16_t icb_partition;
    uint8_t audio_type;
};

struct radio_udf_ro_file_reader {
    const struct radio_udf_ro_file *file;
    struct radio_udf_ro_extent extents[RADIO_UDF_MAX_FILE_EXTENTS];
    size_t extent_count;
    size_t extent_index;
    uint64_t extent_offset;
    uint64_t bytes_remaining;
    uint8_t embedded[RADIO_UDF_BLOCK_BYTES];
    size_t embedded_size;
    size_t embedded_offset;
};

struct radio_udf_ro_reader {
    radio_udf_read_blocks_fn read_blocks;
    void *read_opaque;
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
    struct radio_udf_ro_file files[RADIO_DISC_MAX_ENTRIES];
    size_t file_count;
    uint32_t visited_lbn[RADIO_UDF_MAX_DIRECTORIES];
    uint16_t visited_partition[RADIO_UDF_MAX_DIRECTORIES];
    size_t visited_count;
    size_t entries_seen;
    size_t directories_seen;
    size_t files_seen;
};

static uint16_t radio_udf_le16(const uint8_t *p)
{
    return (uint16_t)((uint16_t)p[0] | ((uint16_t)p[1] << 8));
}

static uint32_t radio_udf_le32(const uint8_t *p)
{
    return (uint32_t)p[0] | ((uint32_t)p[1] << 8) |
           ((uint32_t)p[2] << 16) | ((uint32_t)p[3] << 24);
}

static uint64_t radio_udf_le64(const uint8_t *p)
{
    return (uint64_t)radio_udf_le32(p) |
           ((uint64_t)radio_udf_le32(p + 4U) << 32);
}

static int radio_udf_tag_valid(const uint8_t *block, uint16_t expected_id)
{
    if (radio_udf_le16(block) != expected_id)
        return 0;
    uint8_t checksum = 0U;
    for (size_t i = 0U; i < 16U; ++i)
        if (i != 4U)
            checksum = (uint8_t)(checksum + block[i]);
    return checksum == block[4];
}

static int radio_udf_read_block(struct radio_udf_ro_reader *reader,
                                uint32_t lba, uint8_t *block)
{
    if (reader == NULL || reader->read_blocks == NULL || block == NULL ||
        lba > reader->last_lba)
        return -1;
    return reader->read_blocks(reader->read_opaque, lba, 1U, block);
}

static int radio_udf_resolve_lbn(const struct radio_udf_ro_reader *reader,
                                 uint16_t partition_ref, uint32_t lbn,
                                 uint32_t *physical_lbn)
{
    uint32_t resolved = lbn;
    if (partition_ref == reader->logical_partition_ref &&
        reader->virtual_partition)
    {
        if (reader->vat_entries == NULL || (size_t)lbn >= reader->vat_entry_count)
            return -1;
        resolved = radio_udf_le32(reader->vat_entries + (size_t)lbn * 4U);
        if (resolved >= 0xfffffff0U)
            return -1;
    }
    else if (partition_ref != reader->logical_partition_ref &&
             partition_ref != reader->physical_partition_ref)
        return -1;
    if (resolved >= reader->partition_length ||
        (uint64_t)reader->partition_start + resolved > reader->last_lba)
        return -1;
    *physical_lbn = resolved;
    return 0;
}

static int radio_udf_read_partition_range(struct radio_udf_ro_reader *reader,
                                          uint16_t partition_ref, uint32_t lbn,
                                          uint64_t offset, uint8_t *destination,
                                          size_t length)
{
    uint8_t blocks_buffer[RADIO_UDF_READ_BATCH_BLOCKS * RADIO_UDF_BLOCK_BYTES];
    size_t copied = 0U;
    while (copied < length)
    {
        const uint64_t logical64 = (uint64_t)lbn + offset / RADIO_UDF_BLOCK_BYTES;
        if (logical64 > UINT32_MAX)
            return -1;
        const uint32_t logical = (uint32_t)logical64;
        const size_t in_block = (size_t)(offset % RADIO_UDF_BLOCK_BYTES);
        uint32_t first_physical = 0U;
        if (radio_udf_resolve_lbn(reader, partition_ref, logical,
                                  &first_physical) != 0)
            return -1;
        size_t wanted = length - copied;
        const size_t max_bytes = sizeof(blocks_buffer) - in_block;
        if (wanted > max_bytes)
            wanted = max_bytes;
        const size_t blocks_needed = (in_block + wanted + RADIO_UDF_BLOCK_BYTES - 1U) /
                                     RADIO_UDF_BLOCK_BYTES;
        size_t run_blocks = 1U;
        while (run_blocks < blocks_needed)
        {
            const uint64_t next_logical64 = logical64 + run_blocks;
            if (next_logical64 > UINT32_MAX)
                break;
            uint32_t next_physical = 0U;
            if (radio_udf_resolve_lbn(reader, partition_ref,
                    (uint32_t)next_logical64, &next_physical) != 0 ||
                (uint64_t)next_physical != (uint64_t)first_physical + run_blocks)
                break;
            ++run_blocks;
        }
        const uint64_t absolute64 = (uint64_t)reader->partition_start + first_physical;
        if (absolute64 + run_blocks - 1U > reader->last_lba ||
            reader->read_blocks(reader->read_opaque, (uint32_t)absolute64,
                                 (uint16_t)run_blocks, blocks_buffer) != 0)
            return -1;
        size_t available = run_blocks * RADIO_UDF_BLOCK_BYTES - in_block;
        size_t take = wanted < available ? wanted : available;
        memcpy(destination + copied, blocks_buffer + in_block, take);
        copied += take;
        offset += take;
    }
    return 0;
}

static int radio_udf_file_layout(const uint8_t *entry, uint32_t *base,
                                 uint32_t *ea_offset, uint32_t *ad_offset)
{
    const uint16_t tag = radio_udf_le16(entry);
    if (tag == 261U)
    {
        *base = 176U;
        *ea_offset = 168U;
        *ad_offset = 172U;
        return 0;
    }
    if (tag == 266U)
    {
        *base = 216U;
        *ea_offset = 208U;
        *ad_offset = 212U;
        return 0;
    }
    return -1;
}

static int radio_udf_file_reader_open(struct radio_udf_ro_reader *reader,
                                      const struct radio_udf_ro_file *file,
                                      struct radio_udf_ro_file_reader *out)
{
    uint8_t entry[RADIO_UDF_BLOCK_BYTES];
    if (reader == NULL || file == NULL || out == NULL ||
        radio_udf_read_partition_range(reader, file->icb_partition,
            file->icb_lbn, 0U, entry, sizeof(entry)) != 0)
        return -1;
    const uint16_t tag = radio_udf_le16(entry);
    if ((tag != 261U && tag != 266U) || !radio_udf_tag_valid(entry, tag) ||
        (entry[27U] != 4U && entry[27U] != 5U))
        return -1;
    uint32_t base = 0U, ea_field = 0U, ad_field = 0U;
    if (radio_udf_file_layout(entry, &base, &ea_field, &ad_field) != 0)
        return -1;
    const uint64_t information_length = radio_udf_le64(entry + 56U);
    const uint32_t ea_length = radio_udf_le32(entry + ea_field);
    const uint32_t ad_length = radio_udf_le32(entry + ad_field);
    const uint64_t allocation_offset = (uint64_t)base + ea_length;
    const unsigned allocation_type = radio_udf_le16(entry + 34U) & 0x07U;
    if (information_length != file->size || allocation_offset > RADIO_UDF_BLOCK_BYTES ||
        ad_length > RADIO_UDF_BLOCK_BYTES - allocation_offset)
        return -1;

    memset(out, 0, sizeof(*out));
    out->file = file;
    out->bytes_remaining = information_length;
    if (allocation_type == 3U)
    {
        if (information_length > RADIO_UDF_BLOCK_BYTES - allocation_offset)
            return -1;
        memcpy(out->embedded, entry + allocation_offset, (size_t)information_length);
        out->embedded_size = (size_t)information_length;
        return 0;
    }
    if ((allocation_type != 0U && allocation_type != 1U) ||
        (allocation_type == 0U && ad_length % 8U != 0U) ||
        (allocation_type == 1U && ad_length % 16U != 0U))
        return -1;

    const uint32_t stride = allocation_type == 1U ? 16U : 8U;
    uint64_t allocated_bytes = 0U;
    for (uint32_t at = 0U; at < ad_length; at += stride)
    {
        const uint8_t *ad = entry + allocation_offset + at;
        const uint32_t extent_word = radio_udf_le32(ad);
        const uint32_t extent_bytes = extent_word & 0x3fffffffU;
        if (extent_bytes == 0U)
            continue;
        if ((extent_word >> 30) != 0U ||
            out->extent_count >= RADIO_UDF_MAX_FILE_EXTENTS)
            return -1;
        struct radio_udf_ro_extent *extent = &out->extents[out->extent_count++];
        extent->bytes = extent_bytes;
        extent->lbn = radio_udf_le32(ad + 4U);
        extent->partition_ref = allocation_type == 1U
                                    ? radio_udf_le16(ad + 8U)
                                    : reader->logical_partition_ref;
        allocated_bytes += extent_bytes;
    }
    if (allocated_bytes < information_length || out->extent_count == 0U)
        return -1;
    for (size_t i = 0U; i < out->extent_count; ++i)
    {
        const struct radio_udf_ro_extent *extent = &out->extents[i];
        const uint64_t blocks = ((uint64_t)extent->bytes + RADIO_UDF_BLOCK_BYTES - 1U) /
                                RADIO_UDF_BLOCK_BYTES;
        const uint64_t last_logical = (uint64_t)extent->lbn + blocks - 1U;
        uint32_t first_physical = 0U, last_physical = 0U;
        if (blocks == 0U || last_logical > UINT32_MAX ||
            radio_udf_resolve_lbn(reader, extent->partition_ref, extent->lbn,
                                  &first_physical) != 0 ||
            radio_udf_resolve_lbn(reader, extent->partition_ref,
                                  (uint32_t)last_logical, &last_physical) != 0)
            return -1;
        (void)first_physical;
        (void)last_physical;
    }
    return 0;
}

static int radio_udf_file_reader_next(struct radio_udf_ro_reader *reader,
                                      struct radio_udf_ro_file_reader *file_reader,
                                      uint8_t *destination, size_t capacity,
                                      size_t *read_size)
{
    if (reader == NULL || file_reader == NULL || destination == NULL ||
        read_size == NULL)
        return -1;
    *read_size = 0U;
    if (file_reader->bytes_remaining == 0U)
        return 0;
    if (file_reader->embedded_size != 0U)
    {
        size_t take = file_reader->embedded_size - file_reader->embedded_offset;
        if (take > capacity)
            take = capacity;
        memcpy(destination, file_reader->embedded + file_reader->embedded_offset, take);
        file_reader->embedded_offset += take;
        file_reader->bytes_remaining -= take;
        *read_size = take;
        return 0;
    }
    while (*read_size < capacity && file_reader->bytes_remaining != 0U)
    {
        if (file_reader->extent_index >= file_reader->extent_count)
            return -1;
        struct radio_udf_ro_extent *extent =
            &file_reader->extents[file_reader->extent_index];
        if (file_reader->extent_offset >= extent->bytes)
        {
            ++file_reader->extent_index;
            file_reader->extent_offset = 0U;
            continue;
        }
        uint64_t wanted = extent->bytes - file_reader->extent_offset;
        if (wanted > file_reader->bytes_remaining)
            wanted = file_reader->bytes_remaining;
        if (wanted > capacity - *read_size)
            wanted = capacity - *read_size;
        if (wanted > RADIO_UDF_READ_BATCH_BLOCKS * RADIO_UDF_BLOCK_BYTES)
            wanted = RADIO_UDF_READ_BATCH_BLOCKS * RADIO_UDF_BLOCK_BYTES;
        if (wanted == 0U ||
            radio_udf_read_partition_range(reader, extent->partition_ref,
                extent->lbn, file_reader->extent_offset,
                destination + *read_size, (size_t)wanted) != 0)
            return -1;
        file_reader->extent_offset += wanted;
        file_reader->bytes_remaining -= wanted;
        *read_size += (size_t)wanted;
    }
    return 0;
}

static void radio_udf_decode_name(const uint8_t *source, size_t length,
                                  char *destination, size_t capacity)
{
    if (capacity == 0U)
        return;
    size_t written = 0U;
    destination[0] = '\0';
    if (length == 0U)
        return;
    if (source[0] == 8U)
    {
        for (size_t i = 1U; i < length && written + 1U < capacity; ++i)
        {
            const uint8_t c = source[i];
            destination[written++] = c >= 0x20U && c < 0x7fU ? (char)c : '?';
        }
    }
    else if (source[0] == 16U)
    {
        for (size_t i = 1U; i + 1U < length && written + 1U < capacity; i += 2U)
        {
            const uint16_t c = (uint16_t)(((uint16_t)source[i] << 8) | source[i + 1U]);
            destination[written++] = c >= 0x20U && c < 0x7fU ? (char)c : '?';
        }
    }
    destination[written] = '\0';
}

static uint8_t radio_udf_audio_type(const char *name)
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

static int radio_udf_add_visited(struct radio_udf_ro_reader *reader,
                                 uint16_t partition_ref, uint32_t lbn)
{
    for (size_t i = 0U; i < reader->visited_count; ++i)
        if (reader->visited_lbn[i] == lbn &&
            reader->visited_partition[i] == partition_ref)
            return 0;
    if (reader->visited_count >= RADIO_UDF_MAX_DIRECTORIES)
        return -1;
    reader->visited_lbn[reader->visited_count] = lbn;
    reader->visited_partition[reader->visited_count++] = partition_ref;
    return 1;
}

static int radio_udf_scan_directory(struct radio_udf_ro_reader *reader,
                                    uint16_t partition_ref, uint32_t lbn,
                                    const char *path, unsigned depth);

static int radio_udf_load_directory(struct radio_udf_ro_reader *reader,
                                    const struct radio_udf_ro_file *file,
                                    uint8_t **contents, size_t *length)
{
    *contents = NULL;
    *length = 0U;
    if (file->size == 0U || file->size > RADIO_UDF_MAX_DIRECTORY_BYTES)
        return -1;
    struct radio_udf_ro_file_reader file_reader;
    if (radio_udf_file_reader_open(reader, file, &file_reader) != 0)
        return -1;
    uint8_t *data = (uint8_t *)malloc((size_t)file->size);
    if (data == NULL)
        return -1;
    size_t copied = 0U;
    while (copied < (size_t)file->size)
    {
        size_t amount = 0U;
        if (radio_udf_file_reader_next(reader, &file_reader, data + copied,
                                       (size_t)file->size - copied, &amount) != 0 ||
            amount == 0U)
        {
            free(data);
            return -1;
        }
        copied += amount;
    }
    *contents = data;
    *length = copied;
    return 0;
}

static int radio_udf_read_file_entry(struct radio_udf_ro_reader *reader,
                                     uint16_t partition_ref, uint32_t lbn,
                                     uint8_t *entry)
{
    if (radio_udf_read_partition_range(reader, partition_ref, lbn, 0U,
            entry, RADIO_UDF_BLOCK_BYTES) != 0)
        return -1;
    const uint16_t tag = radio_udf_le16(entry);
    return ((tag == 261U || tag == 266U) && radio_udf_tag_valid(entry, tag)) ? 0 : -1;
}

static int radio_udf_scan_directory(struct radio_udf_ro_reader *reader,
                                    uint16_t partition_ref, uint32_t lbn,
                                    const char *path, unsigned depth)
{
    if (depth > RADIO_UDF_MAX_DIRECTORY_DEPTH ||
        reader->entries_seen >= RADIO_UDF_MAX_ENTRIES)
        return 0;
    const int visited = radio_udf_add_visited(reader, partition_ref, lbn);
    if (visited < 0)
        return -1;
    if (visited == 0)
        return 0;
    uint8_t entry[RADIO_UDF_BLOCK_BYTES];
    if (radio_udf_read_file_entry(reader, partition_ref, lbn, entry) != 0 ||
        entry[27U] != 4U)
        return -1;
    struct radio_udf_ro_file dir;
    memset(&dir, 0, sizeof(dir));
    dir.icb_lbn = lbn;
    dir.icb_partition = partition_ref;
    dir.size = radio_udf_le64(entry + 56U);
    ++reader->directories_seen;
    uint8_t *contents = NULL;
    size_t contents_size = 0U;
    if (radio_udf_load_directory(reader, &dir, &contents, &contents_size) != 0)
        return -1;
    size_t offset = 0U;
    int result = 0;
    while (offset + 38U <= contents_size &&
           reader->entries_seen < RADIO_UDF_MAX_ENTRIES)
    {
        const uint8_t *fid = contents + offset;
        if (radio_udf_le16(fid) == 0U)
            break;
        if (radio_udf_le16(fid) != 257U || !radio_udf_tag_valid(fid, 257U))
        {
            result = -1;
            break;
        }
        const uint8_t name_length = fid[19U];
        const uint16_t implementation_length = radio_udf_le16(fid + 36U);
        const size_t descriptor_length = 38U + implementation_length + name_length;
        const size_t aligned = (descriptor_length + 3U) & ~(size_t)3U;
        if (descriptor_length > contents_size - offset || aligned > contents_size - offset)
        {
            result = -1;
            break;
        }
        const uint8_t characteristics = fid[18U];
        if ((characteristics & 0x0cU) == 0U && name_length != 0U)
        {
            char name[RADIO_UDF_NAME_BYTES];
            char child_path[RADIO_UDF_PATH_BYTES];
            radio_udf_decode_name(fid + 38U + implementation_length, name_length,
                                  name, sizeof(name));
            const int path_length = path[0] == '\0'
                ? snprintf(child_path, sizeof(child_path), "%s", name)
                : snprintf(child_path, sizeof(child_path), "%s/%s", path, name);
            if (path_length > 0 && (size_t)path_length < sizeof(child_path))
            {
                const uint32_t child_lbn = radio_udf_le32(fid + 24U);
                const uint16_t child_partition = radio_udf_le16(fid + 28U);
                ++reader->entries_seen;
                if ((characteristics & 0x02U) != 0U)
                {
                    if (depth < RADIO_UDF_MAX_DIRECTORY_DEPTH &&
                        radio_udf_scan_directory(reader, child_partition, child_lbn,
                                                 child_path, depth + 1U) != 0)
                        result = -1;
                }
                else
                {
                    ++reader->files_seen;
                    const uint8_t type = radio_udf_audio_type(name);
                    uint8_t child_entry[RADIO_UDF_BLOCK_BYTES];
                    if (type != 0U && reader->file_count < RADIO_DISC_MAX_ENTRIES &&
                        radio_udf_read_file_entry(reader, child_partition,
                                                  child_lbn, child_entry) == 0 &&
                        child_entry[27U] == 5U)
                    {
                        const uint64_t file_size = radio_udf_le64(child_entry + 56U);
                        if (file_size != 0U)
                        {
                            struct radio_udf_ro_file candidate;
                            memset(&candidate, 0, sizeof(candidate));
                            const char *slash = strrchr(child_path, '/');
                            const char *display = slash != NULL ? slash + 1U : child_path;
                            (void)snprintf(candidate.path, sizeof(candidate.path), "%s", child_path);
                            (void)snprintf(candidate.name, sizeof(candidate.name), "%s", display);
                            candidate.size = file_size;
                            candidate.icb_lbn = child_lbn;
                            candidate.icb_partition = child_partition;
                            candidate.audio_type = type;
                            struct radio_udf_ro_file_reader check_reader;
                            if (radio_udf_file_reader_open(reader, &candidate,
                                                           &check_reader) == 0)
                                reader->files[reader->file_count++] = candidate;
                        }
                    }
                }
            }
        }
        offset += aligned;
    }
    free(contents);
    return result;
}

static int radio_udf_load_vat(struct radio_udf_ro_reader *reader)
{
    for (uint32_t distance = 0U; distance < 4U && distance <= reader->last_lba; ++distance)
    {
        const uint32_t lba = reader->last_lba - distance;
        uint8_t entry[RADIO_UDF_BLOCK_BYTES];
        if (radio_udf_read_block(reader, lba, entry) != 0)
            continue;
        const uint16_t tag = radio_udf_le16(entry);
        if ((tag != 261U && tag != 266U) || !radio_udf_tag_valid(entry, tag) ||
            entry[27U] != 0xf8U)
            continue;
        uint32_t base = 0U, ea_field = 0U, ad_field = 0U;
        if (radio_udf_file_layout(entry, &base, &ea_field, &ad_field) != 0)
            return -1;
        const uint64_t length64 = radio_udf_le64(entry + 56U);
        const uint32_t ea_length = radio_udf_le32(entry + ea_field);
        const uint32_t ad_length = radio_udf_le32(entry + ad_field);
        const uint64_t data_offset = (uint64_t)base + ea_length;
        const unsigned allocation_type = radio_udf_le16(entry + 34U) & 0x07U;
        if (length64 < 152U || length64 > RADIO_UDF_MAX_VAT_BYTES ||
            allocation_type != 3U || data_offset > RADIO_UDF_BLOCK_BYTES ||
            length64 > RADIO_UDF_BLOCK_BYTES - data_offset ||
            ad_length > RADIO_UDF_BLOCK_BYTES - data_offset)
            return -1;
        uint8_t *vat = (uint8_t *)malloc((size_t)length64);
        if (vat == NULL)
            return -1;
        memcpy(vat, entry + data_offset, (size_t)length64);
        const uint16_t header_length = radio_udf_le16(vat);
        if (header_length < 152U || header_length > length64 ||
            ((size_t)length64 - header_length) % 4U != 0U)
        {
            free(vat);
            return -1;
        }
        const size_t count = ((size_t)length64 - header_length) / 4U;
        if (count == 0U)
        {
            free(vat);
            return -1;
        }
        reader->vat_storage = vat;
        reader->vat_entries = vat + header_length;
        reader->vat_entry_count = count;
        reader->virtual_partition = 1;
        return 0;
    }
    return -1;
}

static int radio_udf_scan_fileset(struct radio_udf_ro_reader *reader,
                                  uint32_t fsd_lbn, uint16_t fsd_partition)
{
    uint8_t fileset[RADIO_UDF_BLOCK_BYTES];
    if (radio_udf_read_partition_range(reader, fsd_partition, fsd_lbn,
            0U, fileset, sizeof(fileset)) != 0 ||
        !radio_udf_tag_valid(fileset, 256U))
        return -1;
    const uint32_t root_lbn = radio_udf_le32(fileset + 404U);
    const uint16_t root_partition = radio_udf_le16(fileset + 408U);
    const uint32_t root_extent_bytes = radio_udf_le32(fileset + 400U) & 0x3fffffffU;
    if (root_partition != fsd_partition || root_lbn >= reader->partition_length ||
        root_extent_bytes < 176U)
        return -1;
    uint8_t root_entry[RADIO_UDF_BLOCK_BYTES];
    if (radio_udf_read_file_entry(reader, root_partition, root_lbn, root_entry) != 0 ||
        root_entry[27U] != 4U)
        return -1;
    return radio_udf_scan_directory(reader, root_partition, root_lbn, "", 0U);
}

static int radio_udf_parse_vds(struct radio_udf_ro_reader *reader,
                               uint32_t vds_lba, uint32_t vds_bytes)
{
    const uint64_t blocks = ((uint64_t)vds_bytes + RADIO_UDF_BLOCK_BYTES - 1U) /
                            RADIO_UDF_BLOCK_BYTES;
    if (vds_bytes == 0U || blocks == 0U || blocks > RADIO_UDF_MAX_VDS_BLOCKS ||
        (uint64_t)vds_lba + blocks > (uint64_t)reader->last_lba + 1U)
        return -1;
    int have_partition = 0, have_lvd = 0, type1_match = 0, virtual_match = 0;
    uint16_t partition_number = 0U, fsd_partition = 0U;
    uint16_t physical_ref = 0xffffU, mapped_partition = 0xffffU;
    uint32_t partition_start = 0U, partition_length = 0U, fsd_lbn = 0U;
    for (uint64_t i = 0U; i < blocks; ++i)
    {
        uint8_t block[RADIO_UDF_BLOCK_BYTES];
        const uint32_t lba = vds_lba + (uint32_t)i;
        if (radio_udf_read_block(reader, lba, block) != 0)
            return -1;
        const uint16_t tag = radio_udf_le16(block);
        if (tag == 8U)
            break;
        if (!radio_udf_tag_valid(block, tag))
            continue;
        if (tag == 5U)
        {
            partition_number = radio_udf_le16(block + 22U);
            partition_start = radio_udf_le32(block + 188U);
            partition_length = radio_udf_le32(block + 192U);
            have_partition = 1;
        }
        else if (tag == 6U)
        {
            if (radio_udf_le32(block + 212U) != RADIO_UDF_BLOCK_BYTES)
                return -1;
            fsd_lbn = radio_udf_le32(block + 252U);
            fsd_partition = radio_udf_le16(block + 256U);
            const uint32_t map_length = radio_udf_le32(block + 264U);
            const uint32_t map_count = radio_udf_le32(block + 268U);
            if (map_length > RADIO_UDF_BLOCK_BYTES - 440U || map_count > 64U)
                return -1;
            uint32_t consumed = 0U;
            for (uint32_t map = 0U; map < map_count && consumed < map_length; ++map)
            {
                const uint32_t at = 440U + consumed;
                const uint8_t type = block[at];
                const uint8_t length = block[at + 1U];
                if (length < 2U || length > map_length - consumed)
                    return -1;
                if (type == 1U && length == 6U)
                {
                    const uint16_t mapped = radio_udf_le16(block + at + 4U);
                    if (mapped == partition_number)
                    {
                        type1_match = 1;
                        physical_ref = (uint16_t)map;
                        mapped_partition = mapped;
                    }
                }
                else if (type == 2U && length == 64U)
                {
                    char entity[24];
                    memcpy(entity, block + at + 5U, 23U);
                    entity[23] = '\0';
                    const uint16_t mapped = radio_udf_le16(block + at + 38U);
                    if (map == fsd_partition && mapped == partition_number &&
                        strncmp(entity, "*UDF Virtual Partition", 22U) == 0)
                    {
                        virtual_match = 1;
                        mapped_partition = mapped;
                    }
                }
                consumed += length;
            }
            if (consumed != map_length || fsd_partition >= map_count)
                return -1;
            have_lvd = 1;
        }
    }
    if (!have_partition || !have_lvd || (!type1_match && !virtual_match) ||
        mapped_partition != partition_number || partition_length == 0U ||
        partition_start > reader->last_lba || fsd_lbn >= partition_length)
        return -1;
    reader->partition_number = partition_number;
    reader->partition_start = partition_start;
    reader->partition_length = partition_length;
    reader->logical_partition_ref = fsd_partition;
    reader->physical_partition_ref = physical_ref;
    reader->virtual_partition = 0;
    if (virtual_match && radio_udf_load_vat(reader) != 0)
        return -1;
    if (radio_udf_scan_fileset(reader, fsd_lbn, fsd_partition) != 0)
        return -1;
    return 0;
}

static void radio_udf_ro_close(struct radio_udf_ro_reader *reader)
{
    if (reader == NULL)
        return;
    free(reader->vat_storage);
    memset(reader, 0, sizeof(*reader));
}

static int radio_udf_ro_scan(struct radio_udf_ro_reader *reader,
                             radio_udf_read_blocks_fn read_blocks, void *opaque,
                             uint32_t last_lba, uint32_t data_track_start)
{
    if (reader == NULL || read_blocks == NULL || last_lba < 256U)
        return -1;
    radio_udf_ro_close(reader);
    reader->read_blocks = read_blocks;
    reader->read_opaque = opaque;
    reader->last_lba = last_lba;
    uint32_t candidates[4];
    size_t count = 0U;
    if (data_track_start <= UINT32_MAX - 256U && data_track_start + 256U <= last_lba)
        candidates[count++] = data_track_start + 256U;
    candidates[count++] = 256U;
    candidates[count++] = last_lba - 256U;
    candidates[count++] = last_lba;
    for (size_t i = 0U; i < count; ++i)
    {
        int duplicate = 0;
        for (size_t j = 0U; j < i; ++j)
            if (candidates[j] == candidates[i])
                duplicate = 1;
        if (duplicate)
            continue;
        uint8_t anchor[RADIO_UDF_BLOCK_BYTES];
        if (radio_udf_read_block(reader, candidates[i], anchor) != 0 ||
            !radio_udf_tag_valid(anchor, 2U))
            continue;
        const uint32_t main_bytes = radio_udf_le32(anchor + 16U) & 0x3fffffffU;
        const uint32_t main_lba = radio_udf_le32(anchor + 20U);
        if (radio_udf_parse_vds(reader, main_lba, main_bytes) == 0)
            return 0;
        radio_udf_ro_close(reader);
        reader->read_blocks = read_blocks;
        reader->read_opaque = opaque;
        reader->last_lba = last_lba;
        const uint32_t reserve_bytes = radio_udf_le32(anchor + 24U) & 0x3fffffffU;
        const uint32_t reserve_lba = radio_udf_le32(anchor + 28U);
        if (radio_udf_parse_vds(reader, reserve_lba, reserve_bytes) == 0)
            return 0;
        radio_udf_ro_close(reader);
        reader->read_blocks = read_blocks;
        reader->read_opaque = opaque;
        reader->last_lba = last_lba;
    }
    return -1;
}

#endif
