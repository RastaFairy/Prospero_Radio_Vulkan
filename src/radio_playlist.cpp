// ProsperoRadio - Native PlayStation 5 radio application.
// Copyright (C) 2026 BlackBearReloaded
// SPDX-License-Identifier: GPL-3.0-or-later

#include "radio_playlist.hpp"

#include <stdbool.h>
#include <stdio.h>
#include <string.h>

struct text_slice_t
{
    const char *data;
    size_t size;
};

static char ascii_lower(char value)
{
    return value >= 'A' && value <= 'Z' ? (char)(value + ('a' - 'A')) : value;
}

static bool equal_case(text_slice_t value, const char *text)
{
    const size_t length = strlen(text);
    if (value.size != length)
        return false;
    for (size_t i = 0; i < length; ++i)
    {
        if (ascii_lower(value.data[i]) != ascii_lower(text[i]))
            return false;
    }
    return true;
}

static bool starts_case(text_slice_t value, const char *text)
{
    const size_t length = strlen(text);
    if (value.size < length)
        return false;
    for (size_t i = 0; i < length; ++i)
    {
        if (ascii_lower(value.data[i]) != ascii_lower(text[i]))
            return false;
    }
    return true;
}

static bool contains_case(const char *data, size_t size, const char *text)
{
    const size_t length = strlen(text);
    if (length == 0 || length > size)
        return false;
    for (size_t at = 0; at + length <= size; ++at)
    {
        size_t matched = 0;
        while (matched < length && ascii_lower(data[at + matched]) == ascii_lower(text[matched]))
        {
            ++matched;
        }
        if (matched == length)
            return true;
    }
    return false;
}

static text_slice_t trim(text_slice_t value)
{
    while (value.size && (value.data[0] == ' ' || value.data[0] == '\t' ||
                          value.data[0] == '\r' || value.data[0] == '\n'))
    {
        ++value.data;
        --value.size;
    }
    while (value.size &&
           (value.data[value.size - 1U] == ' ' || value.data[value.size - 1U] == '\t' ||
            value.data[value.size - 1U] == '\r' || value.data[value.size - 1U] == '\n'))
    {
        --value.size;
    }
    return value;
}

static bool valid_url_text(const char *value, size_t size)
{
    if (size == 0)
        return false;
    for (size_t i = 0; i < size; ++i)
    {
        const unsigned char byte = (unsigned char)value[i];
        if (byte <= 0x20U || byte == 0x7fU || value[i] == '#' || value[i] == '\\')
            return false;
    }
    return true;
}

static bool http_url(text_slice_t value)
{
    return starts_case(value, "http://") || starts_case(value, "https://");
}

static bool extension_is(const char *url, const char *extension)
{
    size_t end = 0;
    while (url[end] != '\0' && url[end] != '?' && url[end] != '#')
        ++end;
    const size_t length = strlen(extension);
    return end >= length && equal_case((text_slice_t){url + end - length, length}, extension);
}

radio_playlist_kind_t radio_playlist_kind_from_url(const char *url)
{
    if (url == nullptr)
        return RADIO_PLAYLIST_NONE;
    if (extension_is(url, ".m3u8"))
        return RADIO_PLAYLIST_HLS;
    if (extension_is(url, ".mpd"))
        return RADIO_PLAYLIST_DASH;
    if (extension_is(url, ".m3u"))
        return RADIO_PLAYLIST_M3U;
    if (extension_is(url, ".pls"))
        return RADIO_PLAYLIST_PLS;
    if (extension_is(url, ".xspf"))
        return RADIO_PLAYLIST_XSPF;
    if (extension_is(url, ".asx") || extension_is(url, ".wax") ||
        extension_is(url, ".wvx"))
        return RADIO_PLAYLIST_ASX;
    return RADIO_PLAYLIST_NONE;
}

radio_playlist_kind_t radio_playlist_kind_from_headers(const char *headers, size_t size)
{
    if (headers == nullptr || size == 0)
        return RADIO_PLAYLIST_NONE;
    if (contains_case(headers, size, "application/vnd.apple.mpegurl"))
        return RADIO_PLAYLIST_HLS;
    if (contains_case(headers, size, "application/dash+xml"))
        return RADIO_PLAYLIST_DASH;
    if (contains_case(headers, size, "audio/x-scpls") ||
        contains_case(headers, size, "application/pls"))
        return RADIO_PLAYLIST_PLS;
    if (contains_case(headers, size, "application/xspf+xml"))
        return RADIO_PLAYLIST_XSPF;
    if (contains_case(headers, size, "video/x-ms-asf") ||
        contains_case(headers, size, "audio/x-ms-wax") ||
        contains_case(headers, size, "video/x-ms-wvx"))
        return RADIO_PLAYLIST_ASX;
    if (contains_case(headers, size, "audio/mpegurl") ||
        contains_case(headers, size, "audio/x-mpegurl") ||
        contains_case(headers, size, "application/x-mpegurl"))
        return RADIO_PLAYLIST_M3U;
    return RADIO_PLAYLIST_NONE;
}

radio_playlist_kind_t radio_playlist_kind_from_body(const char *data, size_t size)
{
    if (data == nullptr || size == 0)
        return RADIO_PLAYLIST_NONE;
    if (size >= 3U && memcmp(data, "\xef\xbb\xbf", 3U) == 0)
    {
        data += 3U;
        size -= 3U;
    }
    if (contains_case(data, size, "#EXT-X-"))
        return RADIO_PLAYLIST_HLS;
    if (contains_case(data, size, "<MPD") || contains_case(data, size, ":MPD"))
        return RADIO_PLAYLIST_DASH;
    if (contains_case(data, size, "[playlist]") && contains_case(data, size, "file1="))
        return RADIO_PLAYLIST_PLS;
    if (contains_case(data, size, "xspf.org/ns") &&
        contains_case(data, size, "<location"))
        return RADIO_PLAYLIST_XSPF;
    if (contains_case(data, size, "<asx") && contains_case(data, size, "<ref"))
        return RADIO_PLAYLIST_ASX;
    if (contains_case(data, size, "#EXTM3U"))
        return RADIO_PLAYLIST_M3U;
    return RADIO_PLAYLIST_NONE;
}

static radio_playlist_result_t copy_url(text_slice_t value, char *output, size_t output_size)
{
    if (!valid_url_text(value.data, value.size))
        return RADIO_PLAYLIST_INVALID;
    if (value.size + 1U > output_size)
        return RADIO_PLAYLIST_LIMIT;
    memcpy(output, value.data, value.size);
    output[value.size] = '\0';
    return RADIO_PLAYLIST_OK;
}

radio_playlist_result_t radio_playlist_resolve_url(const char *base_url, const char *reference,
                                                   char *output, size_t output_size)
{
    if (base_url == nullptr || reference == nullptr || output == nullptr || output_size == 0)
        return RADIO_PLAYLIST_INVALID;

    text_slice_t ref = trim((text_slice_t){reference, strlen(reference)});
    if (!valid_url_text(ref.data, ref.size))
        return RADIO_PLAYLIST_INVALID;
    if (http_url(ref))
        return copy_url(ref, output, output_size);
    for (size_t i = 0; i < ref.size; ++i)
    {
        if (ref.data[i] == ':')
            return RADIO_PLAYLIST_INVALID;
        if (ref.data[i] == '/' || ref.data[i] == '?')
            break;
    }

    const char *scheme = strstr(base_url, "://");
    if (scheme == nullptr || (!starts_case((text_slice_t){base_url, strlen(base_url)}, "http://") &&
                              !starts_case((text_slice_t){base_url, strlen(base_url)}, "https://")))
        return RADIO_PLAYLIST_INVALID;
    const char *authority = scheme + 3;
    const char *base_end = base_url + strlen(base_url);
    const char *path = authority;
    while (path < base_end && *path != '/' && *path != '?')
        ++path;
    if (path == authority)
        return RADIO_PLAYLIST_INVALID;

    size_t prefix = 0;
    bool insert_slash = false;
    if (ref.size >= 2U && ref.data[0] == '/' && ref.data[1] == '/')
    {
        prefix = (size_t)(scheme - base_url) + 1U;
    }
    else if (ref.data[0] == '/')
    {
        prefix = (size_t)(path - base_url);
    }
    else if (ref.data[0] == '?')
    {
        const char *query = path;
        while (query < base_end && *query != '?')
            ++query;
        prefix = (size_t)(query - base_url);
    }
    else
    {
        const char *end = path;
        while (end < base_end && *end != '?')
            ++end;
        const char *slash = end;
        while (slash > path && slash[-1] != '/')
            --slash;
        prefix = slash > path ? (size_t)(slash - base_url) : (size_t)(path - base_url);
        insert_slash = slash == path;
    }

    if (prefix + (insert_slash ? 1U : 0U) + ref.size + 1U > output_size)
        return RADIO_PLAYLIST_LIMIT;
    memcpy(output, base_url, prefix);
    if (insert_slash)
        output[prefix++] = '/';
    memcpy(output + prefix, ref.data, ref.size);
    output[prefix + ref.size] = '\0';
    return http_url((text_slice_t){output, prefix + ref.size}) ? RADIO_PLAYLIST_OK
                                                               : RADIO_PLAYLIST_INVALID;
}

static bool pls_entry(text_slice_t line, text_slice_t *value)
{
    size_t equals = 0;
    while (equals < line.size && line.data[equals] != '=')
        ++equals;
    if (equals == line.size)
        return false;
    text_slice_t key = trim((text_slice_t){line.data, equals});
    if (key.size <= 4U || !starts_case(key, "file"))
        return false;
    for (size_t i = 4U; i < key.size; ++i)
    {
        if (key.data[i] < '0' || key.data[i] > '9')
            return false;
    }
    *value = trim((text_slice_t){line.data + equals + 1U, line.size - equals - 1U});
    return value->size != 0;
}

static bool xml_tag_value(const char *data, size_t size, const char *tag,
                          text_slice_t *value)
{
    char opening[48];
    char closing[48];
    const int opening_size = snprintf(opening, sizeof(opening), "<%s", tag);
    const int closing_size = snprintf(closing, sizeof(closing), "</%s>", tag);
    if (opening_size <= 0 || closing_size <= 0 ||
        (size_t)opening_size >= sizeof(opening) || (size_t)closing_size >= sizeof(closing))
        return false;

    for (size_t start = 0; start < size; ++start)
    {
        if (data[start] != '<' || size - start < (size_t)opening_size)
            continue;
        if (!contains_case(data + start, (size_t)opening_size, opening))
            continue;
        const size_t name_end = start + (size_t)opening_size;
        if (name_end < size && data[name_end] != '>' && data[name_end] != ' ' &&
            data[name_end] != '\t' && data[name_end] != '\r' && data[name_end] != '\n')
            continue;
        size_t content = name_end;
        while (content < size && data[content] != '>')
            ++content;
        if (content == size)
            return false;
        ++content;
        for (size_t end = content; end + (size_t)closing_size <= size; ++end)
        {
            if (data[end] == '<' && contains_case(data + end, (size_t)closing_size, closing))
            {
                *value = trim((text_slice_t){data + content, end - content});
                return value->size != 0;
            }
        }
        return false;
    }
    return false;
}

static int xml_entity_value(text_slice_t value, char *output, size_t capacity)
{
    if (output == nullptr || capacity == 0)
        return -1;
    size_t written = 0;
    for (size_t i = 0; i < value.size;)
    {
        if (value.data[i] != '&')
        {
            if (written + 1U >= capacity)
                return -1;
            output[written++] = value.data[i++];
            continue;
        }
        size_t end = i + 1U;
        while (end < value.size && end - i <= 12U && value.data[end] != ';')
            ++end;
        if (end >= value.size || value.data[end] != ';')
            return -1;
        text_slice_t entity = {value.data + i + 1U, end - i - 1U};
        char decoded = 0;
        if (equal_case(entity, "amp"))
            decoded = '&';
        else if (equal_case(entity, "lt"))
            decoded = '<';
        else if (equal_case(entity, "gt"))
            decoded = '>';
        else if (equal_case(entity, "quot"))
            decoded = '"';
        else if (equal_case(entity, "apos"))
            decoded = '\'';
        else
            return -1;
        if (written + 1U >= capacity)
            return -1;
        output[written++] = decoded;
        i = end + 1U;
    }
    output[written] = '\0';
    return (int)written;
}

static bool asx_reference(const char *data, size_t size, text_slice_t *value)
{
    for (size_t i = 0; i + 4U <= size; ++i)
    {
        if (data[i] != '<' || !starts_case((text_slice_t){data + i, size - i}, "<ref"))
            continue;
        const size_t name_end = i + 4U;
        if (name_end < size && data[name_end] != '>' && data[name_end] != ' ' &&
            data[name_end] != '\t' && data[name_end] != '\r' && data[name_end] != '\n')
            continue;
        size_t end = name_end;
        while (end < size && data[end] != '>')
            ++end;
        if (end == size)
            return false;
        size_t at = name_end;
        while (at < end)
        {
            while (at < end && (data[at] == ' ' || data[at] == '\t' || data[at] == '\r' ||
                                data[at] == '\n' || data[at] == '/'))
                ++at;
            const size_t key_start = at;
            while (at < end && data[at] != '=' && data[at] != ' ' && data[at] != '\t')
                ++at;
            const text_slice_t key = {data + key_start, at - key_start};
            while (at < end && (data[at] == ' ' || data[at] == '\t'))
                ++at;
            if (at == end || data[at] != '=')
                continue;
            ++at;
            while (at < end && (data[at] == ' ' || data[at] == '\t'))
                ++at;
            if (at == end || (data[at] != '\'' && data[at] != '"'))
                return false;
            const char quote = data[at++];
            const size_t value_start = at;
            while (at < end && data[at] != quote)
                ++at;
            if (at == end)
                return false;
            if (equal_case(key, "href"))
            {
                *value = trim((text_slice_t){data + value_start, at - value_start});
                return value->size != 0;
            }
            ++at;
        }
        i = end;
    }
    return false;
}

radio_playlist_result_t radio_playlist_first_url(radio_playlist_kind_t kind, const char *data,
                                                 size_t size, const char *playlist_url,
                                                 char *output, size_t output_size)
{
    if ((kind != RADIO_PLAYLIST_M3U && kind != RADIO_PLAYLIST_PLS &&
         kind != RADIO_PLAYLIST_XSPF && kind != RADIO_PLAYLIST_ASX) || data == nullptr ||
        size == 0 || playlist_url == nullptr || output == nullptr)
        return RADIO_PLAYLIST_INVALID;
    if (radio_playlist_kind_from_body(data, size) == RADIO_PLAYLIST_HLS)
        return RADIO_PLAYLIST_IS_HLS;
    if (memchr(data, '\0', size) != nullptr)
        return RADIO_PLAYLIST_INVALID;

    if (kind == RADIO_PLAYLIST_XSPF || kind == RADIO_PLAYLIST_ASX)
    {
        text_slice_t reference = {nullptr, 0};
        const bool found = kind == RADIO_PLAYLIST_XSPF
                               ? xml_tag_value(data, size, "location", &reference)
                               : asx_reference(data, size, &reference);
        if (!found)
            return RADIO_PLAYLIST_NO_ENTRY;
        char decoded[1024];
        if (xml_entity_value(reference, decoded, sizeof(decoded)) < 0)
            return RADIO_PLAYLIST_INVALID;
        return radio_playlist_resolve_url(playlist_url, decoded, output, output_size);
    }

    size_t position = 0;
    bool first = true;
    while (position < size)
    {
        size_t end = position;
        while (end < size && data[end] != '\n')
            ++end;
        text_slice_t line = trim((text_slice_t){data + position, end - position});
        if (first && line.size >= 3U && memcmp(line.data, "\xef\xbb\xbf", 3U) == 0)
        {
            line.data += 3U;
            line.size -= 3U;
            line = trim(line);
        }
        first = false;

        text_slice_t entry = {nullptr, 0};
        if (kind == RADIO_PLAYLIST_M3U)
        {
            if (line.size != 0 && line.data[0] != '#')
                entry = line;
        }
        else if (pls_entry(line, &entry))
        {
            /* Entry populated by pls_entry. */
        }
        if (entry.size != 0)
        {
            char reference[1024];
            if (entry.size + 1U > sizeof(reference))
                return RADIO_PLAYLIST_LIMIT;
            memcpy(reference, entry.data, entry.size);
            reference[entry.size] = '\0';
            return radio_playlist_resolve_url(playlist_url, reference, output, output_size);
        }
        position = end < size ? end + 1U : size;
    }
    return RADIO_PLAYLIST_NO_ENTRY;
}
