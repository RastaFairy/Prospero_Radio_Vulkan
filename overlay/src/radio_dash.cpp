// ProsperoRadio - Native PlayStation 5 radio application.
// Copyright (C) 2026 BlackBearReloaded
// SPDX-License-Identifier: GPL-3.0-or-later

#include "radio_dash.hpp"

#include "radio_playlist.hpp"

#include <memory>
#include <new>
#include <stdbool.h>
#include <string.h>

#define DASH_MAX_DOCUMENT (128U * 1024U)
#define DASH_MAX_BOX_DEPTH 8U
#define DASH_MAX_SAMPLES 4096U

namespace
{
struct slice_t
{
    const char *data;
    size_t size;
};

struct box_t
{
    size_t begin;
    size_t payload;
    size_t end;
    char type[5];
};

static slice_t trim(slice_t value)
{
    while (value.size != 0U && (value.data[0] == ' ' || value.data[0] == '\t' ||
                                value.data[0] == '\r' || value.data[0] == '\n'))
    {
        ++value.data;
        --value.size;
    }
    while (value.size != 0U && (value.data[value.size - 1U] == ' ' ||
                                value.data[value.size - 1U] == '\t' ||
                                value.data[value.size - 1U] == '\r' ||
                                value.data[value.size - 1U] == '\n'))
        --value.size;
    return value;
}

static bool equal(slice_t value, const char *text)
{
    const size_t length = strlen(text);
    return value.size == length && memcmp(value.data, text, length) == 0;
}

static bool starts(const char *data, size_t size, const char *prefix)
{
    const size_t length = strlen(prefix);
    return size >= length && memcmp(data, prefix, length) == 0;
}

static bool contains_bytes(const char *data, size_t size, const char *needle)
{
    const size_t length = strlen(needle);
    if (length == 0U || length > size)
        return false;
    for (size_t at = 0U; at + length <= size; ++at)
    {
        if (memcmp(data + at, needle, length) == 0)
            return true;
    }
    return false;
}

static bool find_open_tag(const char *data, size_t size, const char *name, size_t from,
                          size_t *begin, size_t *end)
{
    char needle[96];
    const size_t name_size = strlen(name);
    if (name_size + 2U >= sizeof(needle))
        return false;
    needle[0] = '<';
    memcpy(needle + 1U, name, name_size);
    needle[name_size + 1U] = '\0';
    for (size_t at = from; at + name_size + 1U < size; ++at)
    {
        if (data[at] != '<' || !starts(data + at, size - at, needle))
            continue;
        const size_t after_name = at + name_size + 1U;
        if (after_name < size && data[after_name] != ' ' && data[after_name] != '\t' &&
            data[after_name] != '\r' && data[after_name] != '\n' && data[after_name] != '/' &&
            data[after_name] != '>')
            continue;
        size_t close = after_name;
        while (close < size && data[close] != '>')
            ++close;
        if (close == size)
            return false;
        *begin = at;
        *end = close + 1U;
        return true;
    }
    return false;
}

static bool attr(slice_t tag, const char *name, slice_t *out)
{
    const size_t name_size = strlen(name);
    size_t at = 1U;
    while (at < tag.size && tag.data[at] != ' ' && tag.data[at] != '\t' &&
           tag.data[at] != '\r' && tag.data[at] != '\n' && tag.data[at] != '>')
        ++at;
    while (at < tag.size)
    {
        while (at < tag.size && (tag.data[at] == ' ' || tag.data[at] == '\t' ||
                                 tag.data[at] == '\r' || tag.data[at] == '\n' ||
                                 tag.data[at] == '/'))
            ++at;
        const size_t key = at;
        while (at < tag.size && tag.data[at] != '=' && tag.data[at] != '>' &&
               tag.data[at] != ' ' && tag.data[at] != '\t' && tag.data[at] != '\r' &&
               tag.data[at] != '\n')
            ++at;
        const size_t key_size = at - key;
        while (at < tag.size && (tag.data[at] == ' ' || tag.data[at] == '\t' ||
                                 tag.data[at] == '\r' || tag.data[at] == '\n'))
            ++at;
        if (at >= tag.size || tag.data[at] != '=')
            break;
        ++at;
        while (at < tag.size && (tag.data[at] == ' ' || tag.data[at] == '\t'))
            ++at;
        if (at >= tag.size || (tag.data[at] != '\'' && tag.data[at] != '"'))
            return false;
        const char quote = tag.data[at++];
        const size_t value = at;
        while (at < tag.size && tag.data[at] != quote)
            ++at;
        if (at >= tag.size)
            return false;
        if (key_size == name_size && memcmp(tag.data + key, name, name_size) == 0)
        {
            *out = {tag.data + value, at - value};
            return true;
        }
        ++at;
    }
    return false;
}

static bool copy_slice(slice_t value, char *out, size_t capacity)
{
    value = trim(value);
    if (value.size == 0U || value.size >= capacity)
        return false;
    size_t written = 0U;
    for (size_t i = 0U; i < value.size && written + 1U < capacity; ++i)
    {
        if (value.data[i] == '&' && starts(value.data + i, value.size - i, "&amp;"))
        {
            out[written++] = '&';
            i += 4U;
        }
        else
            out[written++] = value.data[i];
    }
    out[written] = '\0';
    return true;
}

static bool parse_u64(slice_t value, uint64_t *out)
{
    value = trim(value);
    if (value.size == 0U)
        return false;
    uint64_t number = 0U;
    for (size_t i = 0U; i < value.size; ++i)
    {
        if (value.data[i] < '0' || value.data[i] > '9')
            return false;
        const uint64_t digit = (uint64_t)(value.data[i] - '0');
        if (number > (UINT64_MAX - digit) / 10U)
            return false;
        number = number * 10U + digit;
    }
    *out = number;
    return true;
}

static bool parse_duration(slice_t value, uint64_t *milliseconds)
{
    value = trim(value);
    if (value.size < 4U || value.data[0] != 'P' || value.data[1] != 'T')
        return false;
    size_t at = 2U;
    double total = 0.0;
    bool have = false;
    while (at < value.size)
    {
        double amount = 0.0;
        bool fraction = false;
        double scale = 0.1;
        while (at < value.size && ((value.data[at] >= '0' && value.data[at] <= '9') ||
                                   value.data[at] == '.'))
        {
            if (value.data[at] == '.')
            {
                if (fraction)
                    return false;
                fraction = true;
            }
            else if (fraction)
            {
                amount += (double)(value.data[at] - '0') * scale;
                scale *= 0.1;
            }
            else
                amount = amount * 10.0 + (double)(value.data[at] - '0');
            ++at;
        }
        if (at >= value.size)
            return false;
        if (value.data[at] == 'H')
            total += amount * 3600.0;
        else if (value.data[at] == 'M')
            total += amount * 60.0;
        else if (value.data[at] == 'S')
            total += amount;
        else
            return false;
        ++at;
        have = true;
    }
    if (!have || total <= 0.0 || total > 86400.0 * 365.0)
        return false;
    *milliseconds = (uint64_t)(total * 1000.0 + 0.5);
    return true;
}

static bool append_number(char *out, size_t capacity, size_t *written, uint64_t value,
                          unsigned width)
{
    char digits[24];
    unsigned count = 0U;
    do
    {
        digits[count++] = (char)('0' + (value % 10U));
        value /= 10U;
    } while (value != 0U && count < sizeof(digits));
    while (count < width && count < sizeof(digits))
        digits[count++] = '0';
    if (*written + count >= capacity)
        return false;
    while (count != 0U)
        out[(*written)++] = digits[--count];
    return true;
}

static bool expand_template(slice_t source, const char *representation, uint64_t bandwidth,
                            uint64_t number, uint64_t time_value, char *out, size_t capacity)
{
    size_t written = 0U;
    for (size_t at = 0U; at < source.size;)
    {
        if (source.data[at] != '$')
        {
            if (written + 1U >= capacity)
                return false;
            out[written++] = source.data[at++];
            continue;
        }
        if (at + 1U < source.size && source.data[at + 1U] == '$')
        {
            if (written + 1U >= capacity)
                return false;
            out[written++] = '$';
            at += 2U;
            continue;
        }
        size_t end = at + 1U;
        while (end < source.size && source.data[end] != '$')
            ++end;
        if (end == source.size)
            return false;
        slice_t token = {source.data + at + 1U, end - at - 1U};
        size_t name_size = 0U;
        while (name_size < token.size && token.data[name_size] != '%')
            ++name_size;
        const slice_t name = {token.data, name_size};
        uint64_t value = 0U;
        bool numeric = false;
        if (equal(name, "Number"))
        {
            value = number;
            numeric = true;
        }
        else if (equal(name, "Time"))
        {
            value = time_value;
            numeric = true;
        }
        else if (equal(name, "Bandwidth"))
        {
            value = bandwidth;
            numeric = true;
        }
        if (numeric)
        {
            unsigned width = 0U;
            if (name_size != token.size)
            {
                size_t i = name_size;
                if (i + 3U > token.size || token.data[i] != '%' || token.data[i + 1U] != '0')
                    return false;
                i += 2U;
                const size_t width_begin = i;
                while (i < token.size && token.data[i] >= '0' && token.data[i] <= '9')
                {
                    const unsigned digit = (unsigned)(token.data[i] - '0');
                    if (width > 12U || width * 10U + digit > 12U)
                        return false;
                    width = width * 10U + digit;
                    ++i;
                }
                if (i == width_begin || i + 1U != token.size || token.data[i] != 'd')
                    return false;
            }
            if (!append_number(out, capacity, &written, value, width))
                return false;
        }
        else if (equal(token, "RepresentationID"))
        {
            const size_t id_size = strlen(representation);
            if (id_size == 0U || written + id_size >= capacity)
                return false;
            memcpy(out + written, representation, id_size);
            written += id_size;
        }
        else
            return false;
        at = end + 1U;
    }
    out[written] = '\0';
    return true;
}

static uint32_t read_be32(const uint8_t *data)
{
    return ((uint32_t)data[0] << 24U) | ((uint32_t)data[1] << 16U) |
           ((uint32_t)data[2] << 8U) | data[3];
}

static uint64_t read_be64(const uint8_t *data)
{
    return ((uint64_t)read_be32(data) << 32U) | read_be32(data + 4U);
}

static bool box_at(const uint8_t *data, size_t size, size_t at, box_t *box)
{
    if (at > size || size - at < 8U)
        return false;
    uint64_t box_size = read_be32(data + at);
    size_t header = 8U;
    if (box_size == 1U)
    {
        if (size - at < 16U)
            return false;
        box_size = read_be64(data + at + 8U);
        header = 16U;
    }
    else if (box_size == 0U)
        box_size = size - at;
    if (box_size < header || box_size > size - at)
        return false;
    box->begin = at;
    box->payload = at + header;
    box->end = at + (size_t)box_size;
    memcpy(box->type, data + at + 4U, 4U);
    box->type[4] = '\0';
    return true;
}

static bool find_box(const uint8_t *data, size_t size, const char *wanted, box_t *found,
                     unsigned depth)
{
    if (depth > DASH_MAX_BOX_DEPTH)
        return false;
    size_t at = 0U;
    while (at < size)
    {
        box_t box;
        if (!box_at(data, size, at, &box))
            return false;
        if (memcmp(box.type, wanted, 4U) == 0)
        {
            *found = box;
            return true;
        }
        const bool container = memcmp(box.type, "moov", 4U) == 0 ||
                               memcmp(box.type, "trak", 4U) == 0 ||
                               memcmp(box.type, "mdia", 4U) == 0 ||
                               memcmp(box.type, "minf", 4U) == 0 ||
                               memcmp(box.type, "stbl", 4U) == 0 ||
                               memcmp(box.type, "mvex", 4U) == 0;
        if (container && find_box(data + box.payload, box.end - box.payload, wanted, found,
                                  depth + 1U))
        {
            found->begin += box.payload;
            found->payload += box.payload;
            found->end += box.payload;
            return true;
        }
        at = box.end;
    }
    return false;
}

static bool descriptor_length(const uint8_t *data, size_t size, size_t *at, size_t *length)
{
    size_t value = 0U;
    for (unsigned i = 0U; i < 4U; ++i)
    {
        if (*at >= size || value > (SIZE_MAX >> 7U))
            return false;
        const uint8_t byte = data[(*at)++];
        value = (value << 7U) | (byte & 0x7fU);
        if ((byte & 0x80U) == 0U)
        {
            *length = value;
            return value <= size - *at;
        }
    }
    return false;
}

static bool find_decoder_specific(const uint8_t *data, size_t size, unsigned depth,
                                  const uint8_t **asc, size_t *asc_size)
{
    if (depth > 4U)
        return false;
    size_t at = 0U;
    while (at < size)
    {
        const uint8_t tag = data[at++];
        size_t length = 0U;
        if (!descriptor_length(data, size, &at, &length))
            return false;
        const uint8_t *payload = data + at;
        if (tag == 0x05U)
        {
            *asc = payload;
            *asc_size = length;
            return length >= 2U;
        }
        size_t nested_at = 0U;
        if (tag == 0x03U)
        {
            if (length < 3U)
                return false;
            nested_at = 3U;
            const uint8_t flags = payload[2];
            if ((flags & 0x80U) != 0U) nested_at += 2U;
            if ((flags & 0x40U) != 0U)
            {
                if (nested_at >= length) return false;
                nested_at += 1U + payload[nested_at];
            }
            if ((flags & 0x20U) != 0U) nested_at += 2U;
        }
        else if (tag == 0x04U)
            nested_at = 13U;
        if (nested_at > length)
            return false;
        if (nested_at < length && find_decoder_specific(payload + nested_at,
                                                        length - nested_at, depth + 1U,
                                                        asc, asc_size))
            return true;
        at += length;
    }
    return false;
}

static bool parse_asc(const uint8_t *data, size_t size, radio_dash_aac_config_t *config)
{
    if (size < 2U)
        return false;
    const unsigned object_type = data[0] >> 3U;
    const unsigned frequency = ((unsigned)(data[0] & 0x07U) << 1U) | (data[1] >> 7U);
    const unsigned channels = (data[1] >> 3U) & 0x0fU;
    static const uint32_t rates[13] = {96000U, 88200U, 64000U, 48000U, 44100U, 32000U, 24000U,
                                       22050U, 16000U, 12000U, 11025U, 8000U, 7350U};
    if (object_type != 2U || frequency >= 13U || channels == 0U || channels > 2U)
        return false;
    config->object_type = (uint8_t)object_type;
    config->frequency_index = (uint8_t)frequency;
    config->channel_configuration = (uint8_t)channels;
    config->sample_rate = rates[frequency];
    return true;
}

static bool sample_entry_esds(const uint8_t *data, size_t size, const uint8_t **asc,
                              size_t *asc_size)
{
    box_t stsd;
    if (!find_box(data, size, "stsd", &stsd, 0U) || stsd.end - stsd.payload < 8U)
        return false;
    const uint32_t count = read_be32(data + stsd.payload + 4U);
    size_t at = stsd.payload + 8U;
    for (uint32_t i = 0U; i < count && at < stsd.end; ++i)
    {
        box_t entry;
        if (!box_at(data, stsd.end, at, &entry))
            return false;
        if (memcmp(entry.type, "mp4a", 4U) == 0)
        {
            const size_t children = entry.payload + 28U;
            if (children > entry.end)
                return false;
            size_t child = children;
            while (child < entry.end)
            {
                box_t nested;
                if (!box_at(data, entry.end, child, &nested))
                    break;
                if (memcmp(nested.type, "esds", 4U) == 0 && nested.end - nested.payload >= 4U)
                    return find_decoder_specific(data + nested.payload + 4U,
                                                 nested.end - nested.payload - 4U, 0U,
                                                 asc, asc_size);
                child = nested.end;
            }
        }
        at = entry.end;
    }
    return false;
}

struct sample_info_t
{
    size_t offset;
    size_t size;
};

static bool parse_tfhd(const uint8_t *data, const box_t &box, uint32_t *default_size)
{
    if (box.end - box.payload < 8U)
        return false;
    const uint32_t flags = ((uint32_t)data[box.payload + 1U] << 16U) |
                           ((uint32_t)data[box.payload + 2U] << 8U) | data[box.payload + 3U];
    size_t at = box.payload + 8U;
    if ((flags & 0x000001U) != 0U)
        return false;
    if ((flags & 0x000002U) != 0U) at += 4U;
    if ((flags & 0x000008U) != 0U) at += 4U;
    if ((flags & 0x000010U) != 0U)
    {
        if (at + 4U > box.end)
            return false;
        *default_size = read_be32(data + at);
        at += 4U;
    }
    return at <= box.end;
}

static bool find_mdat(const uint8_t *data, size_t size, size_t *payload, size_t *end)
{
    size_t at = 0U;
    while (at < size)
    {
        box_t box;
        if (!box_at(data, size, at, &box))
            return false;
        if (memcmp(box.type, "mdat", 4U) == 0)
        {
            *payload = box.payload;
            *end = box.end;
            return true;
        }
        at = box.end;
    }
    return false;
}

static bool parse_trun(const uint8_t *data, const box_t &box, uint32_t default_size,
                       size_t moof_begin, size_t mdat_payload, size_t mdat_end,
                       sample_info_t *samples, size_t *sample_count)
{
    if (box.end - box.payload < 8U)
        return false;
    const uint32_t flags = ((uint32_t)data[box.payload + 1U] << 16U) |
                           ((uint32_t)data[box.payload + 2U] << 8U) | data[box.payload + 3U];
    const uint32_t count = read_be32(data + box.payload + 4U);
    if (count == 0U || count > DASH_MAX_SAMPLES || *sample_count + count > DASH_MAX_SAMPLES)
        return false;
    size_t at = box.payload + 8U;
    size_t data_at = mdat_payload;
    if ((flags & 0x000001U) != 0U)
    {
        if (at + 4U > box.end)
            return false;
        const int32_t relative = (int32_t)read_be32(data + at);
        at += 4U;
        const int64_t absolute = (int64_t)moof_begin + relative;
        if (absolute < (int64_t)mdat_payload || absolute > (int64_t)mdat_end)
            return false;
        data_at = (size_t)absolute;
    }
    if ((flags & 0x000004U) != 0U)
        at += 4U;
    for (uint32_t i = 0U; i < count; ++i)
    {
        if ((flags & 0x000100U) != 0U) at += 4U;
        uint32_t sample_size = default_size;
        if ((flags & 0x000200U) != 0U)
        {
            if (at + 4U > box.end)
                return false;
            sample_size = read_be32(data + at);
            at += 4U;
        }
        if ((flags & 0x000400U) != 0U) at += 4U;
        if ((flags & 0x000800U) != 0U) at += 4U;
        if (at > box.end || sample_size == 0U || data_at > mdat_end ||
            sample_size > mdat_end - data_at)
            return false;
        samples[*sample_count].offset = data_at;
        samples[*sample_count].size = sample_size;
        ++*sample_count;
        data_at += sample_size;
    }
    return true;
}

} // namespace

radio_dash_result_t radio_dash_parse_mpd(const char *data, size_t size, const char *manifest_url,
                                         radio_dash_manifest_t *manifest)
{
    if (data == nullptr || size == 0U || size > DASH_MAX_DOCUMENT || manifest_url == nullptr ||
        manifest == nullptr || memchr(data, '\0', size) != nullptr)
        return RADIO_DASH_INVALID;
    if (contains_bytes(data, size, "ContentProtection") ||
        contains_bytes(data, size, "SegmentBase"))
        return RADIO_DASH_UNSUPPORTED;
    memset(manifest, 0, sizeof(*manifest));
    size_t tag_begin = 0U, tag_end = 0U;
    if (!find_open_tag(data, size, "MPD", 0U, &tag_begin, &tag_end))
        return RADIO_DASH_MALFORMED;
    slice_t mpd_tag = {data + tag_begin, tag_end - tag_begin};
    slice_t type = {nullptr, 0U};
    if (attr(mpd_tag, "type", &type) && !equal(type, "static"))
        return RADIO_DASH_UNSUPPORTED;

    char base_url[RADIO_DASH_URL_BYTES];
    const size_t manifest_url_size = strlen(manifest_url);
    if (manifest_url_size == 0U || manifest_url_size >= sizeof(base_url))
        return RADIO_DASH_LIMIT;
    memcpy(base_url, manifest_url, manifest_url_size + 1U);
    size_t base_begin = 0U, base_open_end = 0U;
    if (find_open_tag(data, size, "BaseURL", tag_end, &base_begin, &base_open_end))
    {
        size_t close = base_open_end;
        while (close + 10U <= size && memcmp(data + close, "</BaseURL>", 10U) != 0)
            ++close;
        if (close + 10U > size)
            return RADIO_DASH_MALFORMED;
        char reference[RADIO_DASH_URL_BYTES];
        if (!copy_slice({data + base_open_end, close - base_open_end}, reference,
                        sizeof(reference)))
            return RADIO_DASH_LIMIT;
        char resolved[RADIO_DASH_URL_BYTES];
        if (radio_playlist_resolve_url(manifest_url, reference, resolved, sizeof(resolved)) !=
            RADIO_PLAYLIST_OK)
            return RADIO_DASH_MALFORMED;
        memcpy(base_url, resolved, strlen(resolved) + 1U);
    }

    size_t adaptation_begin = 0U, adaptation_end = 0U, adaptation_close = 0U;
    size_t representation_end = 0U;
    slice_t adaptation_tag = {nullptr, 0U}, representation_tag = {nullptr, 0U};
    bool audio_representation_found = false;
    size_t adaptation_search = tag_end;
    while (find_open_tag(data, size, "AdaptationSet", adaptation_search, &adaptation_begin,
                         &adaptation_end))
    {
        adaptation_tag = {data + adaptation_begin, adaptation_end - adaptation_begin};
        adaptation_close = adaptation_end;
        while (adaptation_close + 16U <= size &&
               memcmp(data + adaptation_close, "</AdaptationSet>", 16U) != 0)
            ++adaptation_close;
        if (adaptation_close + 16U > size)
            return RADIO_DASH_MALFORMED;

        slice_t mime = {nullptr, 0U};
        slice_t content_type = {nullptr, 0U};
        const bool explicit_non_audio =
            (attr(adaptation_tag, "mimeType", &mime) && !starts(mime.data, mime.size, "audio/")) ||
            (attr(adaptation_tag, "contentType", &content_type) &&
             !equal(content_type, "audio"));
        size_t candidate_rep_begin = 0U, candidate_rep_end = 0U;
        if (!explicit_non_audio &&
            find_open_tag(data, adaptation_close, "Representation", adaptation_end,
                          &candidate_rep_begin, &candidate_rep_end))
        {
            slice_t candidate_rep = {data + candidate_rep_begin,
                                     candidate_rep_end - candidate_rep_begin};
            slice_t candidate_codec = {nullptr, 0U};
            if (!attr(candidate_rep, "codecs", &candidate_codec))
                (void)attr(adaptation_tag, "codecs", &candidate_codec);
            if (candidate_codec.data != nullptr &&
                starts(candidate_codec.data, candidate_codec.size, "mp4a.40.2"))
            {
                representation_end = candidate_rep_end;
                representation_tag = candidate_rep;
                audio_representation_found = true;
                break;
            }
        }
        adaptation_search = adaptation_close + 16U;
    }
    if (!audio_representation_found)
        return RADIO_DASH_UNSUPPORTED;
    slice_t id_value = {nullptr, 0U};
    slice_t bandwidth_value = {nullptr, 0U};
    char representation[128] = "";
    uint64_t bandwidth = 0U;
    if (attr(representation_tag, "id", &id_value) && !copy_slice(id_value, representation,
                                                                   sizeof(representation)))
        return RADIO_DASH_LIMIT;
    if (attr(representation_tag, "bandwidth", &bandwidth_value) &&
        !parse_u64(bandwidth_value, &bandwidth))
        return RADIO_DASH_MALFORMED;

    size_t template_begin = 0U, template_end = 0U;
    if (!find_open_tag(data, adaptation_close, "SegmentTemplate", adaptation_end, &template_begin,
                       &template_end))
    {
        if (!find_open_tag(data, size, "SegmentTemplate", representation_end, &template_begin,
                           &template_end))
            return RADIO_DASH_UNSUPPORTED;
    }
    slice_t template_tag = {data + template_begin, template_end - template_begin};
    slice_t init_attr = {nullptr, 0U}, media_attr = {nullptr, 0U};
    slice_t timescale_attr = {nullptr, 0U}, duration_attr = {nullptr, 0U};
    slice_t start_attr = {nullptr, 0U}, offset_attr = {nullptr, 0U};
    if (!attr(template_tag, "initialization", &init_attr) ||
        !attr(template_tag, "media", &media_attr))
        return RADIO_DASH_MALFORMED;
    uint64_t timescale = 1U, duration = 0U, start_number = 1U;
    if (attr(template_tag, "timescale", &timescale_attr) &&
        (!parse_u64(timescale_attr, &timescale) || timescale == 0U || timescale > 1000000U))
        return RADIO_DASH_MALFORMED;
    if (attr(template_tag, "duration", &duration_attr) &&
        (!parse_u64(duration_attr, &duration) || duration == 0U))
        return RADIO_DASH_MALFORMED;
    if (attr(template_tag, "startNumber", &start_attr) &&
        !parse_u64(start_attr, &start_number))
        return RADIO_DASH_MALFORMED;
    uint64_t presentation_offset = 0U;
    if (attr(template_tag, "presentationTimeOffset", &offset_attr) &&
        (!parse_u64(offset_attr, &presentation_offset) || presentation_offset != 0U))
        return RADIO_DASH_UNSUPPORTED;

    char init_path[RADIO_DASH_URL_BYTES];
    if (!expand_template(init_attr, representation, bandwidth, start_number, 0U, init_path,
                         sizeof(init_path)) ||
        radio_playlist_resolve_url(base_url, init_path, manifest->initialization_url,
                                   sizeof(manifest->initialization_url)) != RADIO_PLAYLIST_OK)
        return RADIO_DASH_MALFORMED;

    uint64_t times[RADIO_DASH_MAX_SEGMENTS];
    uint32_t count = 0U;
    size_t timeline_begin = 0U, timeline_open_end = 0U;
    if (find_open_tag(data, size, "SegmentTimeline", template_end, &timeline_begin,
                      &timeline_open_end))
    {
        size_t timeline_close = timeline_open_end;
        while (timeline_close + 18U <= size &&
               memcmp(data + timeline_close, "</SegmentTimeline>", 18U) != 0)
            ++timeline_close;
        if (timeline_close + 18U > size)
            return RADIO_DASH_MALFORMED;
        uint64_t current_time = 0U;
        size_t search = timeline_open_end;
        while (search < timeline_close)
        {
            size_t s_begin = 0U, s_end = 0U;
            if (!find_open_tag(data, timeline_close, "S", search, &s_begin, &s_end))
                break;
            slice_t s_tag = {data + s_begin, s_end - s_begin};
            slice_t d_attr = {nullptr, 0U}, r_attr = {nullptr, 0U}, t_attr = {nullptr, 0U};
            uint64_t d = 0U, repeat = 0U;
            if (!attr(s_tag, "d", &d_attr) || !parse_u64(d_attr, &d) || d == 0U)
                return RADIO_DASH_MALFORMED;
            if (attr(s_tag, "t", &t_attr) && !parse_u64(t_attr, &current_time))
                return RADIO_DASH_MALFORMED;
            if (attr(s_tag, "r", &r_attr))
            {
                if (!parse_u64(r_attr, &repeat))
                    return RADIO_DASH_UNSUPPORTED;
            }
            if (repeat >= RADIO_DASH_MAX_SEGMENTS || count + repeat + 1U > RADIO_DASH_MAX_SEGMENTS)
                return RADIO_DASH_LIMIT;
            for (uint64_t i = 0U; i <= repeat; ++i)
            {
                times[count++] = current_time;
                if (UINT64_MAX - current_time < d)
                    return RADIO_DASH_LIMIT;
                current_time += d;
            }
            search = s_end;
        }
    }
    else
    {
        if (duration == 0U)
            return RADIO_DASH_UNSUPPORTED;
        slice_t duration_text = {nullptr, 0U};
        uint64_t total_ms = 0U;
        if (!attr(mpd_tag, "mediaPresentationDuration", &duration_text) ||
            !parse_duration(duration_text, &total_ms))
            return RADIO_DASH_UNSUPPORTED;
        if (total_ms > (UINT64_MAX - 999U) / timescale)
            return RADIO_DASH_LIMIT;
        const uint64_t scaled = (total_ms * timescale + 999U) / 1000U;
        const uint64_t segment_count = scaled / duration + (scaled % duration != 0U ? 1U : 0U);
        if (segment_count == 0U || segment_count > RADIO_DASH_MAX_SEGMENTS)
            return RADIO_DASH_LIMIT;
        count = (uint32_t)segment_count;
        for (uint32_t i = 0U; i < count; ++i)
            times[i] = (uint64_t)i * duration;
    }
    if (count == 0U)
        return RADIO_DASH_UNSUPPORTED;

    for (uint32_t i = 0U; i < count; ++i)
    {
        char path[RADIO_DASH_URL_BYTES];
        if (start_number > UINT64_MAX - i)
            return RADIO_DASH_LIMIT;
        if (!expand_template(media_attr, representation, bandwidth, start_number + i, times[i],
                             path, sizeof(path)) ||
            radio_playlist_resolve_url(base_url, path, manifest->segment_urls[i],
                                       sizeof(manifest->segment_urls[i])) != RADIO_PLAYLIST_OK)
            return RADIO_DASH_MALFORMED;
    }
    manifest->segment_count = count;
    return RADIO_DASH_OK;
}

radio_dash_result_t radio_dash_parse_aac_init(const uint8_t *data, size_t size,
                                              radio_dash_aac_config_t *config)
{
    if (data == nullptr || size < 8U || config == nullptr)
        return RADIO_DASH_INVALID;
    const uint8_t *asc = nullptr;
    size_t asc_size = 0U;
    if (!sample_entry_esds(data, size, &asc, &asc_size) || !parse_asc(asc, asc_size, config))
        return RADIO_DASH_UNSUPPORTED;
    return RADIO_DASH_OK;
}

radio_dash_result_t radio_dash_fragment_to_adts(const uint8_t *fragment, size_t fragment_size,
                                                const radio_dash_aac_config_t *config,
                                                uint8_t *output, size_t output_capacity,
                                                size_t *output_size)
{
    if (fragment == nullptr || fragment_size < 16U || config == nullptr || output == nullptr ||
        output_size == nullptr || config->object_type != 2U || config->frequency_index >= 13U ||
        config->channel_configuration == 0U || config->channel_configuration > 2U)
        return RADIO_DASH_INVALID;
    size_t mdat_payload = 0U, mdat_end = 0U;
    if (!find_mdat(fragment, fragment_size, &mdat_payload, &mdat_end))
        return RADIO_DASH_MALFORMED;
    /* The audio worker uses a small stack. Keep this bounded table on the
     * heap; the former 4096-entry local array reserved 64 KiB per call. */
    std::unique_ptr<sample_info_t[]> samples(
        new (std::nothrow) sample_info_t[DASH_MAX_SAMPLES]);
    if (!samples)
        return RADIO_DASH_LIMIT;
    size_t sample_count = 0U;
    unsigned traf_count = 0U;
    unsigned trun_count = 0U;
    size_t top = 0U;
    while (top < fragment_size)
    {
        box_t moof;
        if (!box_at(fragment, fragment_size, top, &moof))
            return RADIO_DASH_MALFORMED;
        if (memcmp(moof.type, "moof", 4U) == 0)
        {
            size_t traf_at = moof.payload;
            while (traf_at < moof.end)
            {
                box_t traf;
                if (!box_at(fragment, moof.end, traf_at, &traf))
                    return RADIO_DASH_MALFORMED;
                if (memcmp(traf.type, "traf", 4U) == 0)
                {
                    if (++traf_count > 1U)
                        return RADIO_DASH_UNSUPPORTED;
                    uint32_t default_size = 0U;
                    size_t child_at = traf.payload;
                    while (child_at < traf.end)
                    {
                        box_t child;
                        if (!box_at(fragment, traf.end, child_at, &child))
                            return RADIO_DASH_MALFORMED;
                        if (memcmp(child.type, "tfhd", 4U) == 0 &&
                            !parse_tfhd(fragment, child, &default_size))
                            return RADIO_DASH_MALFORMED;
                        child_at = child.end;
                    }
                    child_at = traf.payload;
                    while (child_at < traf.end)
                    {
                        box_t child;
                        if (!box_at(fragment, traf.end, child_at, &child))
                            return RADIO_DASH_MALFORMED;
                        if (memcmp(child.type, "trun", 4U) == 0)
                        {
                            if (++trun_count > 1U ||
                                !parse_trun(fragment, child, default_size, moof.begin,
                                            mdat_payload, mdat_end, samples.get(), &sample_count))
                                return RADIO_DASH_UNSUPPORTED;
                        }
                        child_at = child.end;
                    }
                }
                traf_at = traf.end;
            }
        }
        top = moof.end;
    }
    if (sample_count == 0U)
        return RADIO_DASH_UNSUPPORTED;

    size_t written = 0U;
    for (size_t i = 0U; i < sample_count; ++i)
    {
        const size_t frame_length = samples[i].size + 7U;
        if (frame_length > 0x1fffU || written > output_capacity ||
            frame_length > output_capacity - written)
            return RADIO_DASH_LIMIT;
        uint8_t *header = output + written;
        header[0] = 0xffU;
        header[1] = 0xf1U;
        header[2] = (uint8_t)(((config->object_type - 1U) << 6U) |
                              (config->frequency_index << 2U) |
                              (config->channel_configuration >> 2U));
        header[3] = (uint8_t)(((config->channel_configuration & 3U) << 6U) |
                              ((frame_length >> 11U) & 3U));
        header[4] = (uint8_t)(frame_length >> 3U);
        header[5] = (uint8_t)(((frame_length & 7U) << 5U) | 0x1fU);
        header[6] = 0xfcU;
        memcpy(header + 7U, fragment + samples[i].offset, samples[i].size);
        written += frame_length;
    }
    *output_size = written;
    return RADIO_DASH_OK;
}
