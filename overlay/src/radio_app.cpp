// ProsperoRadio - Native PlayStation 5 radio application.
// Copyright (C) 2026 BlackBearReloaded
// SPDX-License-Identifier: GPL-3.0-or-later
//
// Physical-radio frontend (01.000.019). The cabinet artwork carries seven
// printed buttons and two dials, so the software behaves like the hardware it
// imitates: the D-pad is the finger that moves across the buttons, Cross
// presses them, the right dial tunes and the left dial is volume. The smoked
// glass shows exactly one surface: now playing, a station list, genres,
// search, AUX or EQ.

#include "radio_app.hpp"
#include "radio_ime.hpp"
#include "payload_probe.hpp"

#include <RmlUi/Core/Element.h>
#include <RmlUi/Core/ElementDocument.h>

#include <algorithm>
#include <cctype>
#include <cmath>
#include <cstdint>
#include <climits>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <map>
#include <string>
#include <string_view>
#include <unordered_set>
#include <utility>
#include <strings.h>

extern "C" uint64_t SDL_GetTicks64(void);
extern "C" uint64_t SDL_GetTicks(void);

namespace {

constexpr unsigned kButtonCount = 7;
constexpr unsigned kListRows = 7;
constexpr unsigned kDiscRows = 5;
constexpr unsigned kDiscFrameCount = 12;
constexpr unsigned long long kBridgeKeepaliveIntervalMs = 10000ULL;
constexpr unsigned long long kBridgeRetryIntervalMs = 3000ULL;
constexpr unsigned long long kDiscIndexRefreshIntervalMs = 15000ULL;
constexpr unsigned long long kUsbIndexRefreshIntervalMs = 30000ULL;
constexpr unsigned kInvalidStation = ~0U;
constexpr char kPresetFileMagic[8] = {'P', 'R', 'S', 'P', 'R', 'S', '0', '2'};
constexpr char kPresetFileMagicV3[8] = {'P', 'R', 'S', 'P', 'R', 'S', '0', '3'};
constexpr std::uint32_t kPresetFileVersion = 3;
constexpr char kAuxFavoriteFileMagic[8] = {'P', 'R', 'A', 'U', 'X', 'F', '0', '1'};
constexpr std::uint32_t kAuxFavoriteFileVersion = 1;
constexpr unsigned kAuxMaxStations = 4096;
constexpr unsigned long long kAuxDeleteHoldMs = 3000ULL;

struct PresetFileV2
{
    char magic[8];
    std::uint32_t version;
    std::int32_t indices[3];
    char uuids[3][40];
};
static_assert(sizeof(PresetFileV2) == 144, "Unexpected preset file layout");

struct PresetFileV3
{
    char magic[8];
    std::uint32_t version;
    std::int32_t indices[3];
    char uuids[3][40];
    std::uint8_t external[3];
    radio_station_t external_stations[3];
};

struct AuxFavoriteFileHeader
{
    char magic[8];
    std::uint32_t version;
    std::uint32_t count;
};

void SetText(Rml::ElementDocument *document, const char *id, const char *value)
{
    if (Rml::Element *element = document->GetElementById(id))
    {
        /* SetInnerRML parses markup. Station names and M3U metadata are remote
         * text, so escape them before inserting them into the live document. */
        const char *text = value ? value : "";
        std::string escaped;
        escaped.reserve(std::strlen(text));
        for (const char *cursor = text; *cursor; ++cursor)
        {
            switch (*cursor)
            {
            case '&': escaped += "&amp;"; break;
            case '<': escaped += "&lt;"; break;
            case '>': escaped += "&gt;"; break;
            default: escaped += *cursor; break;
            }
        }
        element->SetInnerRML(escaped);
    }
}

void SetClass(Rml::ElementDocument *document, const char *id, const char *class_name, bool enabled)
{
    if (Rml::Element *element = document->GetElementById(id))
        element->SetClass(class_name, enabled);
}

void SetVisible(Rml::ElementDocument *document, const char *id, bool visible)
{
    SetClass(document, id, "hidden", !visible);
}

void ShowDiscFrame(Rml::ElementDocument *document, unsigned frame)
{
    for (unsigned index = 0U; index < kDiscFrameCount; ++index)
    {
        char id[32];
        std::snprintf(id, sizeof(id), "cd-disc-icon-%02u", index);
        SetVisible(document, id, index == frame);
    }
}

void SetPixelProperty(Rml::ElementDocument *document, const char *id, const char *name, int value)
{
    char text[24];
    std::snprintf(text, sizeof(text), "%dpx", value);
    if (Rml::Element *element = document->GetElementById(id))
        element->SetProperty(name, text);
}

void FirstValue(const char *values, char *output, std::size_t capacity)
{
    std::size_t count = 0;
    while (values[count] && values[count] != ',' && count + 1 < capacity)
    {
        output[count] = values[count];
        ++count;
    }
    while (count && output[count - 1] == ' ')
        --count;
    output[count] = '\0';
}

bool ContainsCi(const char *text, const char *needle)
{
    if (!*needle)
        return true;
    for (; *text; ++text)
    {
        const char *left = text;
        const char *right = needle;
        while (*left && *right &&
               std::tolower(static_cast<unsigned char>(*left)) ==
                   std::tolower(static_cast<unsigned char>(*right)))
        {
            ++left;
            ++right;
        }
        if (!*right)
            return true;
    }
    return false;
}

bool PlaybackActive(radio_playback_state_t state)
{
    return state == RADIO_PLAYBACK_CONNECTING || state == RADIO_PLAYBACK_BUFFERING ||
           state == RADIO_PLAYBACK_PLAYING || state == RADIO_PLAYBACK_STOPPING;
}

bool IsDiscStation(const std::vector<radio_station_t> &stations, const char *uuid)
{
    if (!uuid || !*uuid)
        return false;
    if (std::strncmp(uuid, "cd-audio-", 9U) == 0 ||
        std::strncmp(uuid, "cd-file-", 8U) == 0)
        return true;
    return std::any_of(stations.begin(), stations.end(), [uuid](const radio_station_t &station) {
        return std::strcmp(station.uuid, uuid) == 0;
    });
}

bool IsUsbStation(const std::vector<radio_station_t> &stations, const char *uuid)
{
    if (!uuid || !*uuid)
        return false;
    if (std::strncmp(uuid, "usb-audio-", 10U) == 0)
        return true;
    return std::any_of(stations.begin(), stations.end(), [uuid](const radio_station_t &station) {
        return std::strcmp(station.uuid, uuid) == 0;
    });
}

bool IsDiscPlaybackActive(const std::vector<radio_station_t> &stations,
                          const radio_service_status_t &status)
{
    if (!PlaybackActive(status.playback_state))
        return false;
    radio_station_t playing{};
    return radio_service_get_playing_station(&playing) && IsDiscStation(stations, playing.uuid);
}

void CopyString(char *destination, std::size_t capacity, const char *source)
{
    if (!capacity)
        return;
    std::size_t count = 0;
    while (source && source[count] && count + 1 < capacity)
    {
        destination[count] = source[count];
        ++count;
    }
    destination[count] = '\0';
}

void TrimLine(char *line)
{
    if (!line)
        return;
    std::size_t length = std::strlen(line);
    while (length && (line[length - 1] == '\r' || line[length - 1] == '\n' ||
                      line[length - 1] == ' ' || line[length - 1] == '\t'))
        line[--length] = '\0';
    char *begin = line;
    while (*begin == ' ' || *begin == '\t')
        ++begin;
    if (begin != line)
        std::memmove(line, begin, std::strlen(begin) + 1);
}

bool IsHttpUrl(const char *text)
{
    return text && (strncasecmp(text, "http://", 7) == 0 ||
                    strncasecmp(text, "https://", 8) == 0);
}

std::uint16_t ReadDiscU16Be(const unsigned char *data)
{
    return static_cast<std::uint16_t>((static_cast<unsigned>(data[0]) << 8) | data[1]);
}

std::uint32_t ReadDiscU32Be(const unsigned char *data)
{
    return (static_cast<std::uint32_t>(data[0]) << 24) |
           (static_cast<std::uint32_t>(data[1]) << 16) |
           (static_cast<std::uint32_t>(data[2]) << 8) | data[3];
}

bool ReadM3uAttribute(const char *extinf, const char *key, char *output,
                      std::size_t capacity)
{
    if (!extinf || !key || !output || capacity == 0 || strncasecmp(extinf, "#EXTINF:", 8) != 0)
        return false;
    const char *cursor = extinf + 8;
    while (*cursor && *cursor != ',')
    {
        while (*cursor == ' ' || *cursor == '\t')
            ++cursor;
        const char *name = cursor;
        while (*cursor && *cursor != '=' && *cursor != ',' && *cursor != ' ' && *cursor != '\t')
            ++cursor;
        const std::size_t name_length = static_cast<std::size_t>(cursor - name);
        if (*cursor != '=')
        {
            while (*cursor && *cursor != ',' && *cursor != ' ' && *cursor != '\t')
                ++cursor;
            continue;
        }
        ++cursor;
        const bool quoted = *cursor == '"';
        if (quoted)
            ++cursor;
        const char *value = cursor;
        if (quoted)
        {
            while (*cursor && *cursor != '"')
                ++cursor;
        }
        else
        {
            while (*cursor && *cursor != ',' && *cursor != ' ' && *cursor != '\t')
                ++cursor;
        }
        if (name_length == std::strlen(key) && strncasecmp(name, key, name_length) == 0)
        {
            const std::size_t value_length = static_cast<std::size_t>(cursor - value);
            const std::size_t copy_length = std::min(value_length, capacity - 1);
            std::memcpy(output, value, copy_length);
            output[copy_length] = '\0';
            return true;
        }
        if (quoted && *cursor == '"')
            ++cursor;
    }
    output[0] = '\0';
    return false;
}

void ReadM3uTitle(const char *extinf, char *output, std::size_t capacity)
{
    if (!extinf || !output || capacity == 0)
        return;
    bool quoted = false;
    for (const char *cursor = extinf + 8; *cursor; ++cursor)
    {
        if (*cursor == '"')
            quoted = !quoted;
        else if (*cursor == ',' && !quoted)
        {
            CopyString(output, capacity, cursor + 1);
            TrimLine(output);
            return;
        }
    }
    output[0] = '\0';
}

void MakeAuxStation(const char *url, const char *extinf, radio_station_t *station)
{
    if (!url || !station)
        return;
    *station = radio_station_t{};
    CopyString(station->url, sizeof(station->url), url);
    char title[256]{};
    char group[128]{};
    char tvg_name[128]{};
    char language[96]{};
    ReadM3uTitle(extinf, title, sizeof(title));
    (void)ReadM3uAttribute(extinf, "tvg-name", tvg_name, sizeof(tvg_name));
    (void)ReadM3uAttribute(extinf, "group-title", group, sizeof(group));
    (void)ReadM3uAttribute(extinf, "tvg-language", language, sizeof(language));
    if (!*title)
        CopyString(title, sizeof(title), *tvg_name ? tvg_name : "Internet radio");
    CopyString(station->name, sizeof(station->name), title);
    CopyString(station->country_code, sizeof(station->country_code), "M3U");
    CopyString(station->country, sizeof(station->country), "External list");
    CopyString(station->tags, sizeof(station->tags), *group ? group : "M3U");
    CopyString(station->language, sizeof(station->language), language);

    const char *end = std::strpbrk(url, "?#");
    const std::size_t url_length = end ? static_cast<std::size_t>(end - url) : std::strlen(url);
    const char *extension = nullptr;
    for (std::size_t i = 0; i < url_length; ++i)
        if (url[i] == '.')
            extension = url + i;
    if (extension && strncasecmp(extension, ".aac", 4) == 0)
        CopyString(station->codec, sizeof(station->codec), "AAC");
    else if (extension && strncasecmp(extension, ".mp3", 4) == 0)
        CopyString(station->codec, sizeof(station->codec), "MP3");
    else if (extension && strncasecmp(extension, ".m3u8", 5) == 0)
    {
        CopyString(station->codec, sizeof(station->codec), "HLS");
        station->hls = 1;
    }
    else if (url_length >= 5U &&
             [&]()
             {
                 for (std::size_t i = 0; i + 5U <= url_length; ++i)
                     if (strncasecmp(url + i, "/mp3/", 5U) == 0)
                         return true;
                 return false;
             }())
        CopyString(station->codec, sizeof(station->codec), "MP3");
    else
        CopyString(station->codec, sizeof(station->codec), "AUTO");

    std::uint64_t hash = UINT64_C(14695981039346656037);
    for (const unsigned char *byte = reinterpret_cast<const unsigned char *>(url); *byte; ++byte)
    {
        hash ^= *byte;
        hash *= UINT64_C(1099511628211);
    }
    std::snprintf(station->uuid, sizeof(station->uuid), "M3U-%016llX",
                  static_cast<unsigned long long>(hash));
}

std::size_t FindCi(std::string_view text, std::string_view needle, std::size_t start = 0)
{
    if (needle.empty() || needle.size() > text.size())
        return std::string_view::npos;
    for (std::size_t at = start; at + needle.size() <= text.size(); ++at)
    {
        std::size_t matched = 0;
        while (matched < needle.size() &&
               std::tolower(static_cast<unsigned char>(text[at + matched])) ==
                   std::tolower(static_cast<unsigned char>(needle[matched])))
            ++matched;
        if (matched == needle.size())
            return at;
    }
    return std::string_view::npos;
}

std::string_view TrimView(std::string_view value)
{
    while (!value.empty() && std::isspace(static_cast<unsigned char>(value.front())))
        value.remove_prefix(1);
    while (!value.empty() && std::isspace(static_cast<unsigned char>(value.back())))
        value.remove_suffix(1);
    return value;
}

void AppendUtf8(std::string &output, unsigned codepoint)
{
    if (codepoint <= 0x7fU)
        output.push_back(static_cast<char>(codepoint));
    else if (codepoint <= 0x7ffU)
    {
        output.push_back(static_cast<char>(0xc0U | (codepoint >> 6)));
        output.push_back(static_cast<char>(0x80U | (codepoint & 0x3fU)));
    }
    else if (codepoint <= 0xffffU && (codepoint < 0xd800U || codepoint > 0xdfffU))
    {
        output.push_back(static_cast<char>(0xe0U | (codepoint >> 12)));
        output.push_back(static_cast<char>(0x80U | ((codepoint >> 6) & 0x3fU)));
        output.push_back(static_cast<char>(0x80U | (codepoint & 0x3fU)));
    }
    else if (codepoint <= 0x10ffffU)
    {
        output.push_back(static_cast<char>(0xf0U | (codepoint >> 18)));
        output.push_back(static_cast<char>(0x80U | ((codepoint >> 12) & 0x3fU)));
        output.push_back(static_cast<char>(0x80U | ((codepoint >> 6) & 0x3fU)));
        output.push_back(static_cast<char>(0x80U | (codepoint & 0x3fU)));
    }
}

bool IsValidUtf8(std::string_view value)
{
    for (std::size_t at = 0U; at < value.size();)
    {
        const unsigned char first = static_cast<unsigned char>(value[at]);
        if (first <= 0x7fU)
        {
            ++at;
            continue;
        }
        unsigned continuation = 0U;
        unsigned codepoint = 0U;
        unsigned minimum = 0U;
        if ((first & 0xe0U) == 0xc0U)
        {
            continuation = 1U;
            codepoint = first & 0x1fU;
            minimum = 0x80U;
        }
        else if ((first & 0xf0U) == 0xe0U)
        {
            continuation = 2U;
            codepoint = first & 0x0fU;
            minimum = 0x800U;
        }
        else if ((first & 0xf8U) == 0xf0U)
        {
            continuation = 3U;
            codepoint = first & 0x07U;
            minimum = 0x10000U;
        }
        else
            return false;
        if (continuation > value.size() - at - 1U)
            return false;
        for (unsigned i = 1U; i <= continuation; ++i)
        {
            const unsigned char next = static_cast<unsigned char>(value[at + i]);
            if ((next & 0xc0U) != 0x80U)
                return false;
            codepoint = (codepoint << 6U) | (next & 0x3fU);
        }
        if (codepoint < minimum || codepoint > 0x10ffffU ||
            (codepoint >= 0xd800U && codepoint <= 0xdfffU))
            return false;
        at += continuation + 1U;
    }
    return true;
}

std::string DecodeWindows1252(std::string_view value)
{
    static const unsigned codepoints[32] = {
        0x20acU, 0xfffdU, 0x201aU, 0x0192U, 0x201eU, 0x2026U, 0x2020U, 0x2021U,
        0x02c6U, 0x2030U, 0x0160U, 0x2039U, 0x0152U, 0xfffdU, 0x017dU, 0xfffdU,
        0xfffdU, 0x2018U, 0x2019U, 0x201cU, 0x201dU, 0x2022U, 0x2013U, 0x2014U,
        0x02dcU, 0x2122U, 0x0161U, 0x203aU, 0x0153U, 0xfffdU, 0x017eU, 0x0178U};
    std::string decoded;
    decoded.reserve(value.size() * 2U);
    for (unsigned char byte : value)
    {
        if (byte < 0x80U)
            decoded.push_back(static_cast<char>(byte));
        else if (byte < 0xa0U)
            AppendUtf8(decoded, codepoints[byte - 0x80U]);
        else
            AppendUtf8(decoded, byte);
    }
    return decoded;
}

bool DecodeXml(std::string_view value, std::string &output)
{
    output.clear();
    output.reserve(value.size());
    for (std::size_t at = 0; at < value.size();)
    {
        if (value[at] != '&')
        {
            output.push_back(value[at++]);
            continue;
        }
        const std::size_t end = value.find(';', at + 1U);
        if (end == std::string_view::npos || end - at > 12U)
            return false;
        const std::string_view entity = value.substr(at + 1U, end - at - 1U);
        if (entity == "amp")
            output.push_back('&');
        else if (entity == "lt")
            output.push_back('<');
        else if (entity == "gt")
            output.push_back('>');
        else if (entity == "quot")
            output.push_back('"');
        else if (entity == "apos")
            output.push_back('\'');
        else if (!entity.empty() && entity.front() == '#')
        {
            unsigned base = 10U;
            std::size_t digit = 1U;
            if (digit < entity.size() && (entity[digit] == 'x' || entity[digit] == 'X'))
            {
                base = 16U;
                ++digit;
            }
            unsigned codepoint = 0U;
            if (digit == entity.size())
                return false;
            for (; digit < entity.size(); ++digit)
            {
                const char c = entity[digit];
                const unsigned value_digit = c >= '0' && c <= '9' ? (unsigned)(c - '0')
                                            : base == 16U && c >= 'a' && c <= 'f'
                                                ? (unsigned)(c - 'a' + 10)
                                            : base == 16U && c >= 'A' && c <= 'F'
                                                ? (unsigned)(c - 'A' + 10)
                                                : base;
                if (value_digit >= base || codepoint > (0x10ffffU - value_digit) / base)
                    return false;
                codepoint = codepoint * base + value_digit;
            }
            AppendUtf8(output, codepoint);
        }
        else
            return false;
        at = end + 1U;
    }
    return true;
}

bool XmlTagContent(std::string_view block, const char *tag, std::string &output)
{
    std::string opening = "<";
    opening += tag;
    const std::size_t start = FindCi(block, opening);
    if (start == std::string_view::npos)
        return false;
    const std::size_t name_end = start + opening.size();
    if (name_end < block.size() && block[name_end] != '>' &&
        !std::isspace(static_cast<unsigned char>(block[name_end])))
        return false;
    const std::size_t content_start = block.find('>', name_end);
    if (content_start == std::string_view::npos)
        return false;
    std::string closing = "</";
    closing += tag;
    const std::size_t end = FindCi(block, closing, content_start + 1U);
    if (end == std::string_view::npos)
        return false;
    const std::string_view content = TrimView(block.substr(content_start + 1U,
                                                            end - content_start - 1U));
    return DecodeXml(content, output);
}

bool XmlAttribute(std::string_view tag, const char *name, std::string &output)
{
    const std::size_t name_pos = FindCi(tag, name);
    if (name_pos == std::string_view::npos)
        return false;
    const std::size_t after_name = name_pos + std::strlen(name);
    if (after_name < tag.size() && tag[after_name] != '=' &&
        !std::isspace(static_cast<unsigned char>(tag[after_name])))
        return false;
    std::size_t equals = after_name;
    while (equals < tag.size() && std::isspace(static_cast<unsigned char>(tag[equals])))
        ++equals;
    if (equals >= tag.size() || tag[equals] != '=')
        return false;
    ++equals;
    while (equals < tag.size() && std::isspace(static_cast<unsigned char>(tag[equals])))
        ++equals;
    if (equals >= tag.size() || (tag[equals] != '\'' && tag[equals] != '"'))
        return false;
    const char quote = tag[equals++];
    const std::size_t end = tag.find(quote, equals);
    return end != std::string_view::npos &&
           DecodeXml(tag.substr(equals, end - equals), output);
}

void AddAuxStation(std::vector<radio_station_t> &stations, const std::string &url,
                   std::string extinf)
{
    if (stations.size() >= kAuxMaxStations || url.size() >= 512U ||
        !IsHttpUrl(url.c_str()) || url.find_first_of(" \t\r\n") != std::string::npos)
        return;
    for (char &character : extinf)
        if (character == '\r' || character == '\n')
            character = ' ';
    radio_station_t station{};
    MakeAuxStation(url.c_str(), extinf.c_str(), &station);
    stations.push_back(station);
}

const radio_facet_t *FindFacet(const std::vector<radio_facet_t> &facets, const char *value)
{
    for (const radio_facet_t &facet : facets)
        if (std::strcmp(facet.value, value) == 0)
            return &facet;
    return nullptr;
}

} // namespace

bool RadioApp::Initialize(Rml::ElementDocument *document)
{
    document_ = document;
    if (!document_)
        return false;
    /* /data is visible to the bundled payload, not to this app sandbox. The
     * loopback bridge restores durable files before SQLite/preferences open. */
    const bool payload_ready = radio_payload_bridge_start();
    payload_bridge_was_ready_ = payload_ready;
    bool state_sync_ok = payload_ready;
    if (payload_ready)
    {
        const auto sync_state = [&state_sync_ok](radio_payload_file_t file, const char *name) {
            bool synchronized = false;
            for (unsigned attempt = 0U; attempt < 2U && !synchronized; ++attempt)
                synchronized = radio_payload_bridge_sync(file);
            if (!synchronized)
            {
                state_sync_ok = false;
                std::fprintf(stderr,
                             "[ProsperoRadio][bridge] startup restore failed file=%s after 2 attempts\n",
                             name);
            }
        };
        sync_state(RADIO_PAYLOAD_CATALOG, "catalog");
        sync_state(RADIO_PAYLOAD_FAVORITES, "favorites");
        sync_state(RADIO_PAYLOAD_EQ, "equalizer");
        sync_state(RADIO_PAYLOAD_PRESETS, "presets");
        sync_state(RADIO_PAYLOAD_AUXFAVORITES, "AUX favorites");
        if (!radio_payload_bridge_pull_report())
            std::fprintf(stderr, "[ProsperoRadio][bridge] startup report fetch failed\n");
    }
    else
        std::fprintf(stderr, "[ProsperoRadio][bridge] startup unavailable; recovery retries enabled\n");
    if (!state_sync_ok)
        std::fprintf(stderr,
                     "[ProsperoRadio][bridge] persistent state was not fully restored before service init; this session uses local cache/defaults\n");
    payload_keepalive_tick_ = SDL_GetTicks64() +
                              (payload_ready ? kBridgeKeepaliveIntervalMs : 1000ULL);
    LoadAuxFavorites();
    radio_service_init();
    service_started_ = true;
    if (payload_ready)
    {
        (void)FetchAuxPlaylist();
        (void)FetchDiscIndex();
        (void)FetchUsbIndex();
    }
    const unsigned long long startup_ticks = SDL_GetTicks64();
    media_index_refresh_tick_ = startup_ticks + kDiscIndexRefreshIntervalMs;
    usb_index_refresh_tick_ = startup_ticks + kUsbIndexRefreshIntervalMs;
    UpdateDiscPresentation();
    RebuildFacets();
    genre_total_ = static_cast<unsigned>(genre_facets_.size());
    ApplyVolumeFrame();
    ApplyTunerFrame();
    LoadEq();
    ApplyButtons();
    BuildList();
    LoadPresets();
    ApplyPresetIndicators();
    RefreshHome();
    RefreshList();
    RefreshGenres();
    eq_preset_ = radio_service_eq_preset();
    RefreshEq();
    return true;
}

void RadioApp::Shutdown()
{
    /* Release the payload-owned LAN socket before shutting down the bridge. */
    (void)radio_payload_bridge_aux_stop();
    if (service_started_)
    {
        radio_service_shutdown();
        service_started_ = false;
    }
    /* Shutdown waits for an active catalog refresh, so the final snapshot
     * cannot race SQLite writes and includes any last-minute preference edit. */
    (void)radio_payload_bridge_push(RADIO_PAYLOAD_CATALOG);
    (void)radio_payload_bridge_push(RADIO_PAYLOAD_FAVORITES);
    (void)radio_payload_bridge_push(RADIO_PAYLOAD_EQ);
    (void)radio_payload_bridge_push(RADIO_PAYLOAD_PRESETS);
    (void)radio_payload_bridge_push(RADIO_PAYLOAD_AUXFAVORITES);
    radio_payload_bridge_shutdown();
    document_ = nullptr;
}

void RadioApp::RebuildFacets()
{
    country_facets_.clear();
    genre_facets_.clear();
    language_facets_.clear();

    auto load = [](radio_facet_kind_t kind, std::vector<radio_facet_t> &facets)
    {
        const unsigned count = radio_service_get_facet_count(kind);
        facets.reserve(count);
        for (unsigned i = 0; i < count; ++i)
        {
            radio_facet_t facet{};
            if (radio_service_get_facet(kind, i, &facet))
                facets.push_back(facet);
        }
    };
    load(RADIO_FACET_GENRE, genre_facets_);

    /* Use the precomputed SQLite facets. A fallback scan through every
     * station becomes thousands of per-row queries for a full catalog. */
    std::sort(genre_facets_.begin(), genre_facets_.end(),
              [](const radio_facet_t &left, const radio_facet_t &right)
              { return left.station_count > right.station_count; });
}

/* --- buttons ---------------------------------------------------------- */

void RadioApp::ShowScreen()
{
    /* Exactly one glass surface per mode: no stacked panels, ever. */
    SetVisible(document_, "screen-home", mode_ == Mode::Home);
    SetVisible(document_, "screen-list", mode_ == Mode::List);
    SetVisible(document_, "screen-genres", mode_ == Mode::Genres);
    SetVisible(document_, "search-panel", false);
    SetVisible(document_, "screen-aux", mode_ == Mode::Aux);
    SetVisible(document_, "screen-barrido", mode_ == Mode::Barrido);
    SetVisible(document_, "screen-eq", mode_ == Mode::Eq);
}

void RadioApp::ApplyButtons()
{
    ShowScreen();
    static const char *names[] = {"home", "radio", "favorites", "genres",
                                  "search", "settings", "play-pause"};
    static const char *states[] = {"normal", "focus", "pressed", "selected", "selected_focus"};
    static const Mode selected_mode[] = {
        Mode::Home,   Mode::List,    Mode::List,     Mode::Aux,
        Mode::Barrido, Mode::Eq,     Mode::Home,
    };
    static const ListKind selected_list[] = {
        ListKind::Radio, ListKind::Radio, ListKind::Favorites, ListKind::Radio,
        ListKind::Radio, ListKind::Radio,  ListKind::Radio,
    };
    radio_service_status_t status{};
    radio_service_get_status(&status);
    for (unsigned i = 0; i < kButtonCount; ++i)
    {
        const bool list_match = selected_mode[i] == Mode::List &&
                                (list_kind_ == selected_list[i] ||
                                 (i == 1U && list_kind_ == ListKind::Usb));
        const bool selected = i != 0 && mode_ == selected_mode[i] &&
                              (selected_mode[i] != Mode::List || list_match) &&
                              !(i == 6 && !PlaybackActive(status.playback_state));
        const bool focused = mode_ == Mode::Home && button_focus_ == i;
        const char *state = selected && focused ? "selected_focus"
                            : selected          ? "selected"
                            : focused           ? "focus"
                                                : "normal";
        for (const char *candidate : states)
        {
            char id[56];
            std::snprintf(id, sizeof(id), "btn-%s-%s", names[i], candidate);
            SetVisible(document_, id, std::strcmp(candidate, state) == 0);
        }
    }
}

void RadioApp::PressButton(unsigned index)
{
    if (mode_ == Mode::Aux && index != 3U)
    {
        const bool stopped = radio_payload_bridge_aux_stop();
        std::fprintf(stderr, "[ProsperoRadio][AUX] stop on view exit %s\n",
                     stopped ? "PASS" : "FAIL");
    }
    switch (index)
    {
    case 0: // POWER — fade del LCD y salida cruda (sin teardown de libc)
        if (poweroff_ticks_ < 0)
        {
            (void)radio_payload_bridge_aux_stop();
            poweroff_ticks_ = 36;
            SetClass(document_, "display", "dying", true);
            SetText(document_, "home-status", "APAGANDO");
        }
        return;
    case 1: // RADIO
        /* Import the durable AUX playlist before showing the source cycle.
         * This lets RADIO -> LEFT/RIGHT reach M3U directly after an upload or
         * application restart, without requiring the separate BARRIDO screen. */
        (void)FetchAuxPlaylist();
        mode_ = Mode::List;
        list_kind_ = ListKind::Radio;
        list_start_ = list_cursor_ = 0;
        BuildList();
        RefreshList();
        ApplyButtons();
        break;
    case 2: // FAVORITES
        (void)FetchAuxPlaylist();
        mode_ = Mode::List;
        list_kind_ = ListKind::Favorites;
        list_start_ = list_cursor_ = 0;
        BuildList();
        RefreshList();
        ApplyButtons();
        break;
    case 3: // AUX — servidor HTTP de listas en el puerto 7000
        mode_ = Mode::Aux;
        radio_service_aux_start();
        RefreshAuxPanel();
        ApplyButtons();
        break;
    case 4: // BARRIDO — revisa la lista subida desde AUX
        mode_ = Mode::Barrido;
        {
            const bool fetched = FetchAuxPlaylist();
            const unsigned stations = static_cast<unsigned>(aux_stations_.size());
            char message[112];
            if (stations > 0)
                std::snprintf(message, sizeof(message), "AUX LISTA | %u EMISORAS | CROSS: ABRIR", stations);
            else if (fetched)
                CopyString(message, sizeof(message), "LISTA RECIBIDA, PERO SIN URLS HTTP VALIDAS");
            else
                CopyString(message, sizeof(message), "SIN LISTA AUX - ENTRA POR AUX");
            SetText(document_, "barrido-status", message);
            std::fprintf(stderr, "[ProsperoRadio][AUX] BARRIDO fetch=%s parsed_entries=%u\n",
                         fetched ? "PASS" : "FAIL", stations);
        }
        ApplyButtons();
        break;
    case 5: // EQ — 12 bandas graficas y faders independientes L/R
        mode_ = Mode::Eq;
        eq_preset_ = radio_service_eq_preset();
        RefreshEq();
        ApplyButtons();
        break;
    case 6: // PLAY / PAUSE
    {
        radio_service_status_t status{};
        radio_service_get_status(&status);
        if (disc_device_available_ && disc_focus_active_)
        {
            if (disc_stations_.empty())
                SetText(document_, "cd-list-status", "NO PLAYABLE TRACKS");
            else if (IsDiscPlaybackActive(disc_stations_, status))
                radio_service_stop();
            else
                PlayDiscStation(disc_stations_[std::min(
                    disc_selected_index_, static_cast<unsigned>(disc_stations_.size() - 1U))]);
            RefreshDiscScreen();
            break;
        }
        if (PlaybackActive(status.playback_state))
            radio_service_stop();
        else if (((tuned_is_aux_ || tuned_is_disc_ || tuned_is_usb_) && tuned_station_valid_) ||
                 status.station_count)
        {
            if (tuned_is_disc_ && tuned_station_valid_)
                (void)radio_service_play_external(&tuned_station_);
            else if (tuned_is_aux_ || tuned_is_usb_)
                (void)radio_service_play_external(&tuned_station_);
            else
                PlayIndex(tuned_index_);
        }
        break;
    }
    default:
        break;
    }
}

/* --- dials ------------------------------------------------------------ */

void RadioApp::AdjustVolume(int direction)
{
    const int current = static_cast<int>(radio_service_get_volume());
    const int next = std::clamp(current + direction * 2, 0, 100);
    if (next != current)
    {
        radio_service_set_volume(static_cast<unsigned>(next));
        ApplyVolumeFrame();
        RefreshVolumeDisplay();
    }
}

void RadioApp::TuneStation(int direction)
{
    preset_active_ = -1;
    tuned_is_aux_ = false;
    tuned_is_disc_ = false;
    tuned_is_usb_ = false;
    ApplyPresetIndicators();
    radio_service_status_t status{};
    radio_service_get_status(&status);
    if (status.station_count == 0U)
        return;
    if (tuned_index_ >= status.station_count)
        tuned_index_ = 0;
    const unsigned next =
        direction < 0 ? (tuned_index_ == 0U ? status.station_count - 1U : tuned_index_ - 1U)
                      : (tuned_index_ + 1U == status.station_count ? 0U : tuned_index_ + 1U);
    tuned_index_ = next;
    tuned_station_valid_ = radio_service_get_station(tuned_index_, &tuned_station_);
    tuner_state_ = direction < 0 ? 1U : 2U;
    ApplyTunerFrame();
    if (PlaybackActive(status.playback_state))
        PlayIndex(tuned_index_);
    RefreshHome();
}

void RadioApp::PlayIndex(unsigned index)
{
    radio_station_t station{};
    if (!radio_service_get_station(index, &station))
        return;
    tuned_station_ = station;
    tuned_station_valid_ = true;
    tuned_is_aux_ = false;
    tuned_is_disc_ = false;
    tuned_is_usb_ = false;
    preset_active_ = -1;
    ApplyPresetIndicators();
    pending_play_uuid_[0] = '\0';
    pending_play_index_ = InvalidStation;
    pending_play_external_ = false;
    radio_service_status_t status{};
    radio_service_get_status(&status);
    if (PlaybackActive(status.playback_state))
    {
        if (radio_service_station_is_playing(index))
            return;
        CopyString(pending_play_uuid_, sizeof(pending_play_uuid_), station.uuid);
        pending_play_index_ = index;
        radio_service_stop();
        return;
    }
    radio_service_play(index);
}

void RadioApp::PlayAuxStation(const radio_station_t &station)
{
    if (!station.uuid[0] || !IsHttpUrl(station.url))
        return;
    tuned_station_ = station;
    tuned_station_valid_ = true;
    tuned_is_aux_ = true;
    tuned_is_disc_ = false;
    tuned_is_usb_ = false;
    preset_active_ = -1;
    ApplyPresetIndicators();
    pending_play_uuid_[0] = '\0';
    pending_play_index_ = InvalidStation;
    pending_play_external_ = false;

    radio_service_status_t status{};
    radio_service_get_status(&status);
    if (PlaybackActive(status.playback_state))
    {
        radio_station_t playing{};
        if (radio_service_get_playing_station(&playing) &&
            std::strcmp(playing.uuid, station.uuid) == 0)
            return;
        pending_play_station_ = station;
        pending_play_external_ = true;
        radio_service_stop();
        return;
    }
    if (!radio_service_play_external(&station))
        SetText(document_, "home-status", "NO SE PUDO ABRIR LA URL M3U");
}

void RadioApp::PlayUsbStation(const radio_station_t &station)
{
    if (!station.uuid[0] || !IsHttpUrl(station.url))
        return;
    tuned_station_ = station;
    tuned_station_valid_ = true;
    tuned_is_aux_ = false;
    tuned_is_disc_ = false;
    tuned_is_usb_ = true;
    preset_active_ = -1;
    ApplyPresetIndicators();
    pending_play_uuid_[0] = '\0';
    pending_play_index_ = InvalidStation;
    pending_play_external_ = false;

    radio_service_status_t status{};
    radio_service_get_status(&status);
    if (PlaybackActive(status.playback_state))
    {
        radio_station_t playing{};
        if (radio_service_get_playing_station(&playing) &&
            std::strcmp(playing.uuid, station.uuid) == 0)
            return;
        pending_play_station_ = station;
        pending_play_external_ = true;
        radio_service_stop();
        return;
    }
    if (!radio_service_play_external(&station))
        SetText(document_, "home-status", "NO SE PUDO ABRIR LA PISTA USB");
}

void RadioApp::StopUsbPlaybackIfUnavailable()
{
    radio_service_status_t status{};
    radio_service_get_status(&status);
    radio_station_t playing{};
    if (PlaybackActive(status.playback_state) &&
        radio_service_get_playing_station(&playing) &&
        IsUsbStation(usb_stations_, playing.uuid))
    {
        radio_service_stop();
        std::fprintf(stderr, "[ProsperoRadio][USB] stop requested; mounted media unavailable\n");
    }
    if (pending_play_external_ &&
        IsUsbStation(usb_stations_, pending_play_station_.uuid))
    {
        pending_play_external_ = false;
        pending_play_station_ = radio_station_t{};
    }
    if (tuned_is_usb_ || IsUsbStation(usb_stations_, tuned_station_.uuid))
    {
        tuned_is_usb_ = false;
        tuned_station_valid_ = false;
    }
}

void RadioApp::PlayDiscStation(const radio_station_t &station)
{
    if (!station.uuid[0] || !IsHttpUrl(station.url))
        return;
    tuned_station_ = station;
    tuned_station_valid_ = true;
    tuned_is_aux_ = false;
    tuned_is_disc_ = true;
    tuned_is_usb_ = false;
    preset_active_ = -1;
    ApplyPresetIndicators();
    pending_play_uuid_[0] = '\0';
    pending_play_index_ = InvalidStation;
    pending_play_external_ = false;

    radio_service_status_t status{};
    radio_service_get_status(&status);
    if (PlaybackActive(status.playback_state))
    {
        radio_station_t playing{};
        if (radio_service_get_playing_station(&playing) &&
            std::strcmp(playing.uuid, station.uuid) == 0)
            return;
        pending_play_station_ = station;
        pending_play_external_ = true;
        radio_service_stop();
        return;
    }
    if (!radio_service_play_external(&station))
        SetText(document_, "home-status", "NO SE PUDO INICIAR LA PISTA DEL CD");
}

void RadioApp::StopDiscPlaybackIfUnavailable()
{
    radio_service_status_t status{};
    radio_service_get_status(&status);
    if (IsDiscPlaybackActive(disc_stations_, status))
    {
        radio_service_stop();
        std::fprintf(stderr, "[ProsperoRadio][CD] stop requested; optical media unavailable\n");
    }
    if (pending_play_external_ &&
        IsDiscStation(disc_stations_, pending_play_station_.uuid))
    {
        pending_play_external_ = false;
        pending_play_station_ = radio_station_t{};
    }
    if (tuned_is_disc_ || IsDiscStation(disc_stations_, tuned_station_.uuid))
    {
        tuned_is_disc_ = false;
        tuned_station_valid_ = false;
    }
}

bool RadioApp::FetchDiscIndex(bool force_refresh)
{
    const bool scan_was_pending = disc_index_pending_;
    const bool device_was_available = disc_device_available_;
    const bool media_was_present = disc_media_present_;
    disc_index_error_ = false;
    disc_index_pending_ = false;
    std::vector<radio_station_t> parsed_stations;
    std::vector<unsigned char> bytes(RADIO_DISC_INDEX_MAX_BYTES);
    std::size_t size = 0U;
    if (!radio_payload_bridge_fetch_disc_index(bytes.data(), bytes.size(), &size,
                                               force_refresh) ||
        size < RADIO_DISC_INDEX_HEADER_BYTES || std::memcmp(bytes.data(), "PRCD", 4U) != 0 ||
        bytes[4] != RADIO_DISC_INDEX_VERSION)
    {
        disc_index_error_ = true;
        std::fprintf(stderr, "[ProsperoRadio][CD] index fetch/format FAIL\n");
        RefreshDiscScreen();
        return false;
    }
    disc_device_available_ =
        bytes[RADIO_DISC_HEADER_DEVICE_AVAILABLE_OFFSET] != 0U;
    const bool device_availability_changed =
        device_was_available != disc_device_available_;
    if (!device_was_available && disc_device_available_)
    {
        disc_focus_active_ = true;
        disc_focus_initialized_ = true;
        std::fprintf(stderr, "[ProsperoRadio][CD] optical device detected; CD surface activated\n");
    }
    if (device_was_available && !disc_device_available_)
        StopDiscPlaybackIfUnavailable();
    UpdateDiscPresentation();
    if (device_availability_changed)
        RefreshHome();
    if (bytes[5] == RADIO_DISC_MEDIA_PENDING && size == RADIO_DISC_INDEX_HEADER_BYTES &&
        ReadDiscU16Be(bytes.data() + 6U) == 0U)
    {
        disc_index_pending_ = true;
        disc_index_poll_tick_ = SDL_GetTicks64() + 750U;
        if (!scan_was_pending)
            std::fprintf(stderr, "[ProsperoRadio][CD] index scan pending\n");
        RefreshDiscScreen();
        return false;
    }
    if (bytes[5] == RADIO_DISC_MEDIA_ERROR)
    {
        disc_index_error_ = true;
        std::fprintf(stderr, "[ProsperoRadio][CD] index scan error\n");
        RefreshDiscScreen();
        return false;
    }
    if (bytes[5] > 3U)
    {
        disc_index_error_ = true;
        std::fprintf(stderr, "[ProsperoRadio][CD] index media kind invalid=%u\n",
                     static_cast<unsigned>(bytes[5]));
        RefreshDiscScreen();
        return false;
    }
    disc_index_poll_tick_ = 0U;
    const unsigned count = ReadDiscU16Be(bytes.data() + 6U);
    if (count > RADIO_DISC_MAX_ENTRIES)
    {
        disc_index_error_ = true;
        std::fprintf(stderr, "[ProsperoRadio][CD] index entry limit FAIL count=%u\n", count);
        RefreshDiscScreen();
        return false;
    }
    if (count != 0U && bytes[5] == 0U)
    {
        disc_index_error_ = true;
        std::fprintf(stderr, "[ProsperoRadio][CD] index status/count mismatch\n");
        RefreshDiscScreen();
        return false;
    }
    std::size_t offset = RADIO_DISC_INDEX_HEADER_BYTES;
    bool seen_tracks[100]{};
    bool seen_files[RADIO_DISC_MAX_ENTRIES]{};
    for (unsigned item = 0U; item < count; ++item)
    {
        if (offset > size || size - offset < RADIO_DISC_INDEX_RECORD_FIXED_BYTES)
        {
            disc_index_error_ = true;
            RefreshDiscScreen();
            return false;
        }
        const unsigned char type = bytes[offset];
        const unsigned id = ReadDiscU16Be(bytes.data() + offset + 1U);
        const unsigned track_number = bytes[offset + 3U];
        const std::uint32_t start_lba = ReadDiscU32Be(bytes.data() + offset + 4U);
        const std::uint32_t end_lba = ReadDiscU32Be(bytes.data() + offset + 8U);
        const unsigned name_size = ReadDiscU16Be(bytes.data() + offset + 12U);
        offset += RADIO_DISC_INDEX_RECORD_FIXED_BYTES;
        if (name_size == 0U || name_size > RADIO_DISC_NAME_BYTES ||
            name_size > size - offset ||
            std::memchr(bytes.data() + offset, '\0', name_size) != nullptr)
        {
            disc_index_error_ = true;
            RefreshDiscScreen();
            return false;
        }
        radio_station_t station{};
        std::memcpy(station.name, bytes.data() + offset,
                    std::min<std::size_t>(name_size, sizeof(station.name) - 1U));
        station.name[std::min<std::size_t>(name_size, sizeof(station.name) - 1U)] = '\0';
        offset += name_size;
        if (type == RADIO_DISC_ENTRY_CDDA)
        {
            if (track_number == 0U || track_number >= 100U || id != track_number ||
                seen_tracks[track_number] || end_lba <= start_lba)
            {
                disc_index_error_ = true;
                RefreshDiscScreen();
                return false;
            }
            seen_tracks[track_number] = true;
            std::snprintf(station.uuid, sizeof(station.uuid), "cd-audio-%03u", track_number);
            std::snprintf(station.url, sizeof(station.url),
                          "http://127.0.0.1:%u/track/%u", RADIO_DISC_HTTP_PORT,
                          track_number);
            CopyString(station.codec, sizeof(station.codec), "CDDA");
            CopyString(station.tags, sizeof(station.tags), "CD-DA");
            station.bitrate = 1411U;
        }
        else if (type == RADIO_DISC_ENTRY_MP3 || type == RADIO_DISC_ENTRY_AAC ||
                 type == RADIO_DISC_ENTRY_FLAC || type == RADIO_DISC_ENTRY_WAV)
        {
            if (id >= RADIO_DISC_MAX_ENTRIES || seen_files[id])
            {
                disc_index_error_ = true;
                RefreshDiscScreen();
                return false;
            }
            seen_files[id] = true;
            std::snprintf(station.uuid, sizeof(station.uuid), "cd-file-%03u", id);
            std::snprintf(station.url, sizeof(station.url),
                          "http://127.0.0.1:%u/file/%u", RADIO_DISC_HTTP_PORT, id);
            const char *codec = type == RADIO_DISC_ENTRY_MP3 ? "MP3"
                                : type == RADIO_DISC_ENTRY_AAC ? "AAC"
                                : type == RADIO_DISC_ENTRY_FLAC ? "FLAC"
                                                               : "WAV";
            CopyString(station.codec, sizeof(station.codec), codec);
            CopyString(station.tags, sizeof(station.tags), "DISC AUDIO");
        }
        else
        {
            disc_index_error_ = true;
            RefreshDiscScreen();
            return false;
        }
        parsed_stations.push_back(station);
    }
    if (offset != size)
    {
        disc_index_error_ = true;
        RefreshDiscScreen();
        return false;
    }
    disc_stations_.swap(parsed_stations);
    disc_media_present_ = bytes[5] != 0U;
    if (media_was_present && !disc_media_present_)
        StopDiscPlaybackIfUnavailable();
    if (media_was_present != disc_media_present_)
        RefreshHome();
    if (disc_selected_index_ >= disc_stations_.size())
        disc_selected_index_ = disc_stations_.empty() ? 0U
                                                       : static_cast<unsigned>(disc_stations_.size() - 1U);
    std::fprintf(stderr, "[ProsperoRadio][CD] index result=PASS media=%u entries=%u\n",
                 static_cast<unsigned>(bytes[5]), count);
    RefreshDiscScreen();
    return true;
}

bool RadioApp::FetchUsbIndex(bool force_refresh)
{
    const bool scan_was_pending = usb_index_pending_;
    const bool device_was_available = usb_device_available_;
    usb_index_error_ = false;
    usb_index_pending_ = false;
    std::vector<radio_station_t> parsed_stations;
    std::vector<unsigned char> bytes(RADIO_USB_INDEX_MAX_BYTES);
    std::size_t size = 0U;
    if (!radio_payload_bridge_fetch_usb_index(bytes.data(), bytes.size(), &size,
                                              force_refresh) ||
        size < RADIO_USB_INDEX_HEADER_BYTES || std::memcmp(bytes.data(), "PRUS", 4U) != 0 ||
        bytes[4] != RADIO_USB_INDEX_VERSION)
    {
        usb_index_error_ = true;
        std::fprintf(stderr, "[ProsperoRadio][USB] index fetch/format FAIL\n");
        if (mode_ == Mode::List && list_kind_ == ListKind::Usb)
            RefreshList();
        return false;
    }
    usb_device_available_ =
        bytes[RADIO_USB_HEADER_DEVICE_AVAILABLE_OFFSET] != 0U;
    if (device_was_available && !usb_device_available_)
    {
        StopUsbPlaybackIfUnavailable();
        RefreshHome();
    }
    if (bytes[5] == RADIO_USB_MEDIA_PENDING && size == RADIO_USB_INDEX_HEADER_BYTES &&
        ReadDiscU16Be(bytes.data() + 6U) == 0U)
    {
        usb_index_pending_ = true;
        usb_index_poll_tick_ = SDL_GetTicks64() + 750U;
        if (!scan_was_pending)
            std::fprintf(stderr, "[ProsperoRadio][USB] index scan pending\n");
        if (mode_ == Mode::List && list_kind_ == ListKind::Usb)
            RefreshList();
        return false;
    }
    if (bytes[5] == RADIO_USB_MEDIA_ERROR)
    {
        usb_index_error_ = true;
        std::fprintf(stderr, "[ProsperoRadio][USB] index scan error\n");
        if (mode_ == Mode::List && list_kind_ == ListKind::Usb)
            RefreshList();
        return false;
    }
    const unsigned count = ReadDiscU16Be(bytes.data() + 6U);
    if (count > RADIO_USB_MAX_ENTRIES || bytes[5] > 2U ||
        (bytes[5] == 0U && usb_device_available_) ||
        (bytes[5] != 0U && !usb_device_available_) ||
        (count != 0U && bytes[5] != 1U) || (count == 0U && bytes[5] == 1U))
    {
        usb_index_error_ = true;
        std::fprintf(stderr, "[ProsperoRadio][USB] index status/count mismatch\n");
        return false;
    }
    std::size_t offset = RADIO_USB_INDEX_HEADER_BYTES;
    bool seen_ids[RADIO_USB_MAX_ENTRIES]{};
    for (unsigned item = 0U; item < count; ++item)
    {
        if (offset > size || size - offset < RADIO_USB_INDEX_RECORD_FIXED_BYTES)
        {
            usb_index_error_ = true;
            return false;
        }
        const unsigned char type = bytes[offset];
        const unsigned id = ReadDiscU16Be(bytes.data() + offset + 1U);
        const unsigned name_size = ReadDiscU16Be(bytes.data() + offset + 12U);
        offset += RADIO_USB_INDEX_RECORD_FIXED_BYTES;
        if (name_size == 0U || name_size > RADIO_USB_NAME_BYTES || name_size > size - offset ||
            std::memchr(bytes.data() + offset, '\0', name_size) != nullptr ||
            id >= RADIO_USB_MAX_ENTRIES || seen_ids[id])
        {
            usb_index_error_ = true;
            return false;
        }
        seen_ids[id] = true;
        radio_station_t station{};
        const std::size_t copied = std::min<std::size_t>(name_size, sizeof(station.name) - 1U);
        std::memcpy(station.name, bytes.data() + offset, copied);
        station.name[copied] = '\0';
        offset += name_size;
        const char *codec = nullptr;
        switch (type)
        {
        case RADIO_USB_ENTRY_MP3: codec = "MP3"; break;
        case RADIO_USB_ENTRY_AAC: codec = "AAC"; break;
        case RADIO_USB_ENTRY_FLAC: codec = "FLAC"; break;
        case RADIO_USB_ENTRY_WAV: codec = "WAV"; break;
        default:
            usb_index_error_ = true;
            return false;
        }
        std::snprintf(station.uuid, sizeof(station.uuid), "usb-audio-%03u", id);
        std::snprintf(station.url, sizeof(station.url), "http://127.0.0.1:%u/usb/%u",
                      RADIO_USB_HTTP_PORT, id);
        CopyString(station.codec, sizeof(station.codec), codec);
        CopyString(station.tags, sizeof(station.tags), "USB Audio");
        CopyString(station.country_code, sizeof(station.country_code), "USB");
        parsed_stations.push_back(station);
    }
    if (offset != size)
    {
        usb_index_error_ = true;
        return false;
    }
    usb_stations_.swap(parsed_stations);
    usb_index_poll_tick_ = 0U;
    std::fprintf(stderr, "[ProsperoRadio][USB] index result=PASS mounted=%u entries=%u\n",
                 static_cast<unsigned>(usb_device_available_), count);
    if (mode_ == Mode::List && list_kind_ == ListKind::Usb)
    {
        BuildList();
        RefreshList();
    }
    return true;
}

void RadioApp::UpdateDiscPresentation()
{
    if (!disc_device_available_)
        disc_focus_active_ = false;
    else if (!disc_focus_initialized_)
    {
        disc_focus_active_ = true;
        disc_focus_initialized_ = true;
    }
    SetVisible(document_, "cd-interface", disc_device_available_);
    SetClass(document_, "cd-interface", "sleeping", !disc_focus_active_);
    SetClass(document_, "app-shell", "cd-stack-active", disc_device_available_);
    SetVisible(document_, "radio-cd-stack-overlay", disc_device_available_);
    if (disc_device_available_ && !disc_stack_loaded_)
        if (Rml::Element *stack = document_->GetElementById("radio-cd-stack-overlay"))
        {
            stack->SetAttribute("src", Rml::String("art/prospero_cd_stack_overlay_2048.tga"));
            disc_stack_loaded_ = true;
        }
    if (disc_device_available_ && !disc_frame_loaded_)
    {
        bool loaded = true;
        for (unsigned index = 0U; index < kDiscFrameCount; ++index)
        {
            char id[32];
            char source[48];
            std::snprintf(id, sizeof(id), "cd-disc-icon-%02u", index);
            std::snprintf(source, sizeof(source), "art/cd_disc_frame_%02u.tga", index);
            if (Rml::Element *disc = document_->GetElementById(id))
                disc->SetAttribute("src", Rml::String(source));
            else
                loaded = false;
        }
        disc_frame_loaded_ = loaded;
        if (loaded)
            ShowDiscFrame(document_, disc_animation_frame_);
    }
    SetText(document_, "home-hint", disc_device_available_
                                         ? "↑ CD  |  ↓ RADIO  |  OPTIONS: SCAN  |  DIAL: TRACK"
                                         : "CROSS: press  |  DIAL: tune  |  SQUARE: favorite");
    RefreshDiscScreen();
}

void RadioApp::RefreshDiscScreen()
{
    if (!document_)
        return;
    SetVisible(document_, "cd-interface", disc_device_available_);
    SetClass(document_, "cd-interface", "sleeping", !disc_focus_active_);
    radio_service_status_t status{};
    radio_service_get_status(&status);
    radio_station_t playing_station{};
    const bool disc_playing = PlaybackActive(status.playback_state) &&
                              radio_service_get_playing_station(&playing_station) &&
                              IsDiscStation(disc_stations_, playing_station.uuid);
    const bool disc_busy = disc_index_pending_ || disc_playing;
    SetVisible(document_, "cd-disc-viewport", disc_busy && disc_focus_active_);
    if (disc_index_error_)
        SetText(document_, "cd-player-status", "READER ERROR");
    else if (disc_index_pending_)
        SetText(document_, "cd-player-status", "READING DISC");
    else if (!disc_media_present_)
        SetText(document_, "cd-player-status", "INSERT AUDIO DISC");
    else if (disc_stations_.empty())
        SetText(document_, "cd-player-status", "NO PLAYABLE TRACKS");
    else if (disc_playing)
        SetText(document_, "cd-player-status", status.playback_state == RADIO_PLAYBACK_CONNECTING
                                                    ? "CONNECTING"
                                                    : status.playback_state == RADIO_PLAYBACK_BUFFERING
                                                          ? "BUFFERING"
                                                          : "PLAYING");
    else
        SetText(document_, "cd-player-status", "DISC READY");

    const unsigned count = static_cast<unsigned>(disc_stations_.size());
    const bool has_cdda_tracks = std::any_of(
        disc_stations_.begin(), disc_stations_.end(), [](const radio_station_t &station) {
            return std::strcmp(station.codec, "CDDA") == 0;
        });
    const bool has_data_files = std::any_of(
        disc_stations_.begin(), disc_stations_.end(), [](const radio_station_t &station) {
            return std::strcmp(station.codec, "CDDA") != 0;
        });
    const unsigned first = disc_selected_index_ >= kDiscRows
                               ? disc_selected_index_ - kDiscRows + 1U
                               : 0U;
    char text[160];
    if (count == 0U)
    {
        SetText(document_, "cd-now-title", disc_index_error_ ? "CHECK READER"
                                                              : disc_index_pending_ ? "READING DISC"
                                                              : disc_media_present_ ? "NO PLAYABLE TRACKS"
                                                                                    : "INSERT AN AUDIO CD");
        SetText(document_, "cd-now-meta", "CD-DA 44.1 kHz  ·  CD-MP3");
        if (disc_index_pending_)
            CopyString(text, sizeof(text), "SCANNING OPTICAL DRIVE");
        else if (disc_index_error_)
            CopyString(text, sizeof(text), "SCAN FAILED");
        else if (disc_media_present_)
            CopyString(text, sizeof(text), "NO SUPPORTED TRACKS");
        else
            CopyString(text, sizeof(text), "INSERT DISC TO BEGIN");
        SetText(document_, "cd-list-status", text);
    }
    else
    {
        const radio_station_t &selected = disc_stations_[disc_selected_index_];
        SetText(document_, "cd-now-title", selected.name);
        if (std::strcmp(selected.codec, "CDDA") == 0)
            std::snprintf(text, sizeof(text), "TRACK %02u / %02u  ·  CD-DA · 44.1 kHz",
                          disc_selected_index_ + 1U, count);
        else
            std::snprintf(text, sizeof(text), "FILE %02u / %02u  ·  DISC %s",
                          disc_selected_index_ + 1U, count, selected.codec);
        SetText(document_, "cd-now-meta", text);
        const char *item_label = has_cdda_tracks && has_data_files
                                     ? "AUDIO ITEMS"
                                     : has_data_files ? "AUDIO FILES" : "CD-DA TRACKS";
        std::snprintf(text, sizeof(text), "%u %s  ·  DIAL SELECTS  ·  ↓ RADIO",
                      count, item_label);
        SetText(document_, "cd-list-status", text);
    }
    for (unsigned row = 0U; row < kDiscRows; ++row)
    {
        char id[40];
        const unsigned index = first + row;
        if (index >= count)
        {
            std::snprintf(id, sizeof(id), "cd-track-%u", row);
            SetVisible(document_, id, false);
            continue;
        }
        const radio_station_t &station = disc_stations_[index];
        std::snprintf(id, sizeof(id), "cd-track-%u", row);
        SetVisible(document_, id, true);
        SetClass(document_, id, "selected", index == disc_selected_index_);
        SetClass(document_, id, "playing", disc_playing &&
                                                std::strcmp(station.uuid, playing_station.uuid) == 0);
        std::snprintf(id, sizeof(id), "cd-track-name-%u", row);
        SetText(document_, id, station.name);
        std::snprintf(id, sizeof(id), "cd-track-kind-%u", row);
        std::snprintf(text, sizeof(text), "%s  %02u/%02u",
                      std::strcmp(station.codec, "CDDA") == 0 ? "CD-DA" : station.codec,
                      index + 1U, count);
        SetText(document_, id, text);
    }
}

void RadioApp::ChangeDiscSelection(int direction)
{
    if (disc_stations_.empty())
        return;
    const unsigned count = static_cast<unsigned>(disc_stations_.size());
    const unsigned next = direction > 0
                              ? std::min(disc_selected_index_ + 1U, count - 1U)
                              : (disc_selected_index_ == 0U ? 0U : disc_selected_index_ - 1U);
    if (next == disc_selected_index_)
        return;
    radio_service_status_t status{};
    radio_service_get_status(&status);
    const bool was_playing_disc = IsDiscPlaybackActive(disc_stations_, status);
    disc_selected_index_ = next;
    if (was_playing_disc)
        PlayDiscStation(disc_stations_[disc_selected_index_]);
    if (was_playing_disc)
        RefreshHome();
    RefreshDiscScreen();
}

/* --- list mode -------------------------------------------------------- */

void RadioApp::BuildList()
{
    for (unsigned &entry : list_indices_)
        entry = kInvalidStation;
    std::fill(list_aux_entries_, list_aux_entries_ + kListRows, false);
    std::fill(list_usb_entries_, list_usb_entries_ + kListRows, false);
    if (list_kind_ == ListKind::Auxiliary)
    {
        list_total_ = static_cast<unsigned>(aux_stations_.size());
        if (list_total_ && list_start_ >= list_total_)
        {
            list_start_ = (list_total_ - 1U) / kListRows * kListRows;
            list_cursor_ = 0;
        }
        for (unsigned row = 0; row < kListRows && list_start_ + row < list_total_; ++row)
        {
            list_indices_[row] = list_start_ + row;
            list_aux_entries_[row] = true;
        }
        return;
    }
    if (list_kind_ == ListKind::Usb)
    {
        list_total_ = static_cast<unsigned>(usb_stations_.size());
        if (list_total_ && list_start_ >= list_total_)
        {
            list_start_ = (list_total_ - 1U) / kListRows * kListRows;
            list_cursor_ = 0;
        }
        for (unsigned row = 0; row < kListRows && list_start_ + row < list_total_; ++row)
        {
            list_indices_[row] = list_start_ + row;
            list_usb_entries_[row] = true;
        }
        return;
    }
    if (list_kind_ == ListKind::Favorites)
    {
        const unsigned radio_count = radio_service_get_favorite_count();
        const unsigned aux_count = static_cast<unsigned>(aux_favorites_.size());
        list_total_ = radio_count + aux_count;
        if (list_total_ && list_start_ >= list_total_)
        {
            list_start_ = (list_total_ - 1U) / kListRows * kListRows;
            list_cursor_ = 0;
        }
        const unsigned radio_rows = list_start_ < radio_count
                                        ? std::min(kListRows, radio_count - list_start_)
                                        : 0U;
        if (radio_rows)
        {
            unsigned actual_total = 0;
            if (!radio_service_query_page(nullptr, RADIO_CATALOG_ORDER_POPULAR, true,
                                          list_start_, radio_rows, &actual_total))
            {
                list_total_ = aux_count;
                list_start_ = list_cursor_ = 0;
                return;
            }
        }
        for (unsigned row = 0; row < kListRows && list_start_ + row < list_total_; ++row)
        {
            const unsigned absolute = list_start_ + row;
            if (absolute < radio_count)
                list_indices_[row] = absolute;
            else
            {
                list_indices_[row] = absolute - radio_count;
                list_aux_entries_[row] = true;
            }
        }
        return;
    }
    radio_catalog_query_t query{};
    radio_catalog_query_t *query_ptr = nullptr;
    if (*filter_genre_)
    {
        CopyString(query.tag, sizeof(query.tag), filter_genre_);
        query_ptr = &query;
    }
    unsigned total = 0;
    const bool loaded =
        radio_service_query_page(query_ptr, RADIO_CATALOG_ORDER_POPULAR,
                                 list_kind_ == ListKind::Favorites, list_start_, kListRows, &total);
    list_total_ = loaded ? total : 0;
    if (list_total_ && list_start_ >= list_total_)
    {
        list_start_ = (list_total_ - 1U) / kListRows * kListRows;
        list_cursor_ = 0;
    }
    if (!loaded)
    {
        list_total_ = 0;
        return;
    }
    for (unsigned row = 0; row < kListRows; ++row)
    {
        if (list_start_ + row >= list_total_)
            break;
        list_indices_[row] = list_start_ + row;
    }
}

void RadioApp::RefreshList()
{
    if (list_total_ == 0U)
        list_cursor_ = 0U;
    else if (list_start_ + list_cursor_ >= list_total_)
        list_cursor_ = std::min(kListRows - 1U, list_total_ - list_start_ - 1U);
    const char *title = list_kind_ == ListKind::Favorites ? "FAVORITES"
                        : list_kind_ == ListKind::Auxiliary ? "AUX"
                        : list_kind_ == ListKind::Usb       ? "USB AUDIO"
                                                           : "RADIO BROWSER";
    SetText(document_, "list-title", title);
    SetClass(document_, "list-tab-radio", "active", list_kind_ == ListKind::Radio);
    SetClass(document_, "list-tab-favorites", "active", list_kind_ == ListKind::Favorites);
    SetClass(document_, "list-tab-aux", "active", list_kind_ == ListKind::Auxiliary);
    SetClass(document_, "list-tab-usb", "active", list_kind_ == ListKind::Usb);
    char text[96];
    if (list_total_ == 0U)
    {
        SetText(document_, "list-status", list_kind_ == ListKind::Favorites
                                               ? "SIN FAVORITOS  |  □ GUARDA"
                                           : list_kind_ == ListKind::Auxiliary
                                               ? "AUX VACIA  |  ENTRA POR AUX"
                                           : list_kind_ == ListKind::Usb
                                               ? usb_index_pending_ ? "LEYENDO UNIDADES USB"
                                               : usb_index_error_ ? "ERROR AL LEER USB"
                                               : usb_device_available_ ? "UNIDAD SIN AUDIO COMPATIBLE"
                                                                       : "CONECTA USB FAT32 / EXFAT"
                                               : "CATALOGO CARGANDO");
        for (unsigned row = 0; row < kListRows; ++row)
        {
            char id[24];
            std::snprintf(id, sizeof(id), "list-row-%u", row);
            SetVisible(document_, id, false);
            std::snprintf(id, sizeof(id), "list-delete-countdown-%u", row);
            SetVisible(document_, id, false);
        }
        return;
    }
    if (list_kind_ == ListKind::Usb && usb_index_error_)
        CopyString(text, sizeof(text), "ERROR AL LEER USB · MOSTRANDO ULTIMA LISTA");
    else if (list_kind_ == ListKind::Usb && usb_index_pending_)
        CopyString(text, sizeof(text), "ACTUALIZANDO USB · ULTIMA LISTA VALIDA");
    else if (list_kind_ == ListKind::Auxiliary)
        std::snprintf(text, sizeof(text), "%u/%u | □ FAV · 3s BORRAR",
                      list_start_ + list_cursor_ + 1U, list_total_);
    else
        std::snprintf(text, sizeof(text), "%u / %u  |  ◀ ▶ FUENTE",
                      list_start_ + list_cursor_ + 1U, list_total_);
    SetText(document_, "list-status", text);
    for (unsigned row = 0; row < kListRows; ++row)
    {
        char id[24];
        std::snprintf(id, sizeof(id), "list-row-%u", row);
        char countdown_id[40];
        std::snprintf(countdown_id, sizeof(countdown_id), "list-delete-countdown-%u", row);
        if (list_start_ + row >= list_total_ || list_indices_[row] == kInvalidStation)
        {
            SetVisible(document_, id, false);
            SetVisible(document_, countdown_id, false);
            continue;
        }
        SetVisible(document_, id, true);
        radio_station_t station{};
        bool have_station = false;
        if (list_usb_entries_[row])
        {
            if (list_indices_[row] < usb_stations_.size())
            {
                station = usb_stations_[list_indices_[row]];
                have_station = true;
            }
        }
        else if (list_aux_entries_[row])
        {
            const auto &source = list_kind_ == ListKind::Favorites ? aux_favorites_ : aux_stations_;
            if (list_indices_[row] < source.size())
            {
                station = source[list_indices_[row]];
                have_station = true;
            }
        }
        else
            have_station = radio_service_get_station(list_indices_[row], &station);
        if (!have_station)
        {
            SetVisible(document_, id, false);
            SetVisible(document_, countdown_id, false);
            continue;
        }
        std::snprintf(id, sizeof(id), "list-name-%u", row);
        SetText(document_, id, station.name);
        SetClass(document_, id, "search-match",
                 *search_query_ && ContainsCi(station.name, search_query_));
        std::snprintf(id, sizeof(id), "list-meta-%u", row);
        char tag[40];
        FirstValue(station.tags, tag, sizeof(tag));
        if (list_usb_entries_[row])
            std::snprintf(text, sizeof(text), "USB AUDIO  |  %s", station.codec);
        else if (list_aux_entries_[row])
            std::snprintf(text, sizeof(text), "AUX  |  %.24s  |  %s", *tag ? tag : "Radio",
                          station.codec);
        else
            std::snprintf(text, sizeof(text), "%s  |  %s %u kbps",
                          *station.country_code ? station.country_code : "WW", *tag ? tag : "Music",
                          station.bitrate);
        SetText(document_, id, text);
        const bool show_delete_countdown = aux_delete_hold_active_ &&
                                           list_kind_ == ListKind::Auxiliary &&
                                           list_cursor_ == row &&
                                           std::strcmp(station.uuid, aux_delete_hold_uuid_) == 0;
        SetVisible(document_, countdown_id, show_delete_countdown);
        if (show_delete_countdown)
        {
            const unsigned long long elapsed = radio_input_milliseconds() - aux_delete_hold_start_;
            const unsigned long long remaining = elapsed >= kAuxDeleteHoldMs
                                                     ? 0ULL
                                                     : kAuxDeleteHoldMs - elapsed;
            const double seconds = static_cast<double>((remaining + 99ULL) / 100ULL) / 10.0;
            std::snprintf(text, sizeof(text), "BORRAR EN %.1f s · SUELTA CANCELA", seconds);
            SetText(document_, countdown_id, text);
        }
        std::snprintf(id, sizeof(id), "list-fav-%u", row);
        SetVisible(document_, id, !show_delete_countdown && !list_usb_entries_[row] &&
                                       (list_aux_entries_[row] ? IsAuxFavorite(station.uuid)
                                                               : radio_service_is_favorite(station.uuid)));
        std::snprintf(id, sizeof(id), "list-meta-%u", row);
        SetVisible(document_, id, !show_delete_countdown);
        std::snprintf(id, sizeof(id), "list-row-%u", row);
        SetClass(document_, id, "cursor", list_cursor_ == row);
    }
}

void RadioApp::RefreshGenres()
{
    char text[96];
    std::snprintf(text, sizeof(text), "%u GENRES", genre_total_);
    SetText(document_, "genres-status", text);
    for (unsigned row = 0; row < kListRows; ++row)
    {
        char id[24];
        std::snprintf(id, sizeof(id), "genre-row-%u", row);
        const unsigned index = genre_start_ + row;
        if (index >= genre_total_)
        {
            SetVisible(document_, id, false);
            continue;
        }
        SetVisible(document_, id, true);
        std::snprintf(id, sizeof(id), "genre-name-%u", row);
        SetText(document_, id, genre_facets_[index].label);
        std::snprintf(id, sizeof(id), "genre-count-%u", row);
        std::snprintf(text, sizeof(text), "%u", genre_facets_[index].station_count);
        SetText(document_, id, text);
        std::snprintf(id, sizeof(id), "genre-row-%u", row);
        SetClass(document_, id, "cursor", genre_cursor_ == row);
    }
}

/* --- home mode -------------------------------------------------------- */

void RadioApp::RefreshHome()
{
    radio_service_status_t status{};
    radio_service_get_status(&status);
    if (disc_device_available_ && disc_focus_active_ && disc_media_present_)
    {
        /* The upper display owns the track list and now-playing metadata while
         * it is selected. Keep the canonical lower LCD on the input label so
         * the same track title is not drawn twice. */
        SetText(document_, "home-name", "CD PLAYER");
        SetText(document_, "home-meta", "USB OPTICAL INPUT");
        return;
    }
    radio_station_t station{};
    bool have = PlaybackActive(status.playback_state) &&
                radio_service_get_playing_station(&station);
    if (!have && tuned_station_valid_)
    {
        station = tuned_station_;
        have = true;
    }
    if (!have)
        have = radio_service_get_station(tuned_index_, &station);
    if (have)
    {
        tuned_station_ = station;
        tuned_station_valid_ = true;
        const bool disc_source = disc_device_available_ && disc_media_present_ &&
                                 (tuned_is_disc_ ||
                                  IsDiscStation(disc_stations_, station.uuid));
        if (disc_source)
        {
            /* The upper CD display owns track details while the lower radio
             * LCD identifies the selected input instead of repeating them. */
            SetText(document_, "home-name", "CD PLAYER");
            SetText(document_, "home-meta", "USB OPTICAL INPUT");
            return;
        }
        SetText(document_, "home-name", station.name);
        char tag[40];
        FirstValue(station.tags, tag, sizeof(tag));
        char text[192];
        std::snprintf(text, sizeof(text), "%s  |  %s  |  %s %u kbps",
                      *station.country_code ? station.country_code : "WW",
                      *station.language ? station.language : "Music", station.codec,
                      station.bitrate);
        SetText(document_, "home-meta", text);
    }
    else
    {
        SetText(document_, "home-name", status.station_count ? "Tuning..." : "No stations");
        SetText(document_, "home-meta", "Turn the TUNING dial to find a station");
    }
}

void RadioApp::RefreshStatus()
{
    radio_service_status_t status{};
    radio_service_get_status(&status);
    const char *text = "READY";
    char line[128];
    bool warning = false;
    bool error = false;
    switch (status.playback_state)
    {
    case RADIO_PLAYBACK_CONNECTING:
        text = "CONNECTING";
        warning = true;
        break;
    case RADIO_PLAYBACK_BUFFERING:
        std::snprintf(line, sizeof(line), "BUFFERING  |  %u HZ  |  %u CH", status.sample_rate,
                      status.channels);
        text = line;
        warning = true;
        break;
    case RADIO_PLAYBACK_PLAYING:
        std::snprintf(line, sizeof(line), "PLAYING  |  %u HZ  |  %u CH", status.sample_rate,
                      status.channels);
        text = line;
        break;
    case RADIO_PLAYBACK_STOPPING:
        text = "STOPPING";
        warning = true;
        break;
    case RADIO_PLAYBACK_ERROR:
        std::snprintf(line, sizeof(line), "ERROR  |  %08x", static_cast<unsigned>(status.error_code));
        text = line;
        error = true;
        break;
    default:
        break;
    }
    SetText(document_, "home-status", text);
    SetClass(document_, "home-status", "warning", warning);
    SetClass(document_, "home-status", "error", error);
    const bool playing = status.playback_state == RADIO_PLAYBACK_PLAYING;
    SetClass(document_, "btn-play-pause", "playing", playing);
}

/* --- search ----------------------------------------------------------- */

void RadioApp::OpenSearch()
{
    if (search_open_)
        return;
    search_open_ = true;
    mode_ = Mode::Search;
    CopyString(search_edit_, sizeof(search_edit_), search_query_);
    search_focus_ = 0;
    UpdateSearch();
    ApplyButtons();
}

void RadioApp::CloseSearch(bool apply)
{
    if (!search_open_)
        return;
    radio_ime_cancel();
    search_open_ = false;
    mode_ = Mode::Home;
    if (apply)
    {
        CopyString(search_query_, sizeof(search_query_), search_edit_);
        mode_ = Mode::List;
        list_kind_ = ListKind::Radio;
        list_start_ = list_cursor_ = 0;
        BuildList();
        RefreshList();
        ApplyButtons();
        radio_catalog_query_t query{};
        CopyString(query.name, sizeof(query.name), search_query_);
        radio_service_search(&query);
        return;
    }
    RefreshHome();
    ApplyButtons();
}

void RadioApp::CycleFilter(unsigned filter, int direction)
{
    if (filter == 0)
    {
        // genre facets double as the search filter list
        const unsigned count = static_cast<unsigned>(genre_facets_.size());
        int current = -1;
        for (unsigned i = 0; i < count; ++i)
            if (std::strcmp(filter_genre_, genre_facets_[i].value) == 0)
                current = static_cast<int>(i);
        int next = current + direction;
        if (next < -1)
            next = static_cast<int>(count) - 1;
        if (next >= static_cast<int>(count))
            next = -1;
        if (next < 0)
            filter_genre_[0] = '\0';
        else
            CopyString(filter_genre_, sizeof(filter_genre_), genre_facets_[next].value);
    }
    else
    {
        static const unsigned rates[] = {0, 64, 128, 192, 256};
        int current = 0;
        for (unsigned i = 0; i < sizeof(rates) / sizeof(rates[0]); ++i)
            if (rates[i] == filter_bitrate_)
                current = static_cast<int>(i);
        current += direction;
        if (current < 0)
            current = static_cast<int>(sizeof(rates) / sizeof(rates[0])) - 1;
        if (current >= static_cast<int>(sizeof(rates) / sizeof(rates[0])))
            current = 0;
        filter_bitrate_ = rates[current];
    }
    UpdateSearch();
}

void RadioApp::UpdateSearch()
{
    SetText(document_, "search-query-label",
            *search_edit_ ? search_edit_ : "Press Cross to type a station or genre...");
    const radio_facet_t *genre = FindFacet(genre_facets_, filter_genre_);
    char text[144];
    std::snprintf(text, sizeof(text), "GENRE  /  %s", genre ? genre->label : "All genres");
    SetText(document_, "filter-label-1", text);
    const char *bitrate = filter_bitrate_ == 0     ? "Any bitrate"
                          : filter_bitrate_ == 64  ? "64+ kbps"
                          : filter_bitrate_ == 128 ? "128+ kbps"
                          : filter_bitrate_ == 192 ? "192+ kbps"
                                                   : "256+ kbps";
    std::snprintf(text, sizeof(text), "QUALITY  /  %s", bitrate);
    SetText(document_, "filter-label-3", text);
}

void RadioApp::HandleSearchKey(radio_input_key_t key)
{
    if (key == RADIO_INPUT_CIRCLE)
    {
        CloseSearch(false);
        return;
    }
    if (key == RADIO_INPUT_OPTIONS)
    {
        radio_service_refresh();
        return;
    }
    if (key == RADIO_INPUT_UP)
        search_focus_ = search_focus_ ? search_focus_ - 1 : 4;
    else if (key == RADIO_INPUT_DOWN)
        search_focus_ = search_focus_ < 4 ? search_focus_ + 1 : 0;
    else if ((key == RADIO_INPUT_LEFT || key == RADIO_INPUT_RIGHT) && search_focus_ >= 1 &&
             search_focus_ <= 3)
        CycleFilter(search_focus_ - 1, key == RADIO_INPUT_LEFT ? -1 : 1);
    else if (key == RADIO_INPUT_CROSS)
    {
        if (search_focus_ == 0)
            radio_ime_request(search_edit_, ImeResult, this);
        else if (search_focus_ <= 3)
            CycleFilter(search_focus_ - 1, 1);
        else
        {
            CopyString(search_query_, sizeof(search_query_), search_edit_);
            CloseSearch(true);
        }
    }
    UpdateFocusSearch();
}

void RadioApp::UpdateFocusSearch()
{
    SetClass(document_, "search-query", "focused", search_focus_ == 0);
    SetClass(document_, "filter-1", "focused", search_focus_ == 1);
    SetClass(document_, "filter-3", "focused", search_focus_ == 3);
    SetClass(document_, "search-apply", "focused", search_focus_ == 4);
}

void RadioApp::ImeResult(const char *text, void *user_data)
{
    RadioApp *app = static_cast<RadioApp *>(user_data);
    if (!app || !text)
        return;
    CopyString(app->search_edit_, sizeof(app->search_edit_), text);
    app->UpdateSearch();
}

/* --- atlas frames ----------------------------------------------------- */

void RadioApp::RefreshVolumeDisplay()
{
    /* Independent of ApplyVolumeFrame: the figure must track the service even
     * when the sprite stays on the same 5% step. */
    char text[32];
    std::snprintf(text, sizeof(text), "VOL %u%%", radio_service_get_volume());
    SetText(document_, "volume-level", text);
}

void RadioApp::ApplyVolumeFrame()
{
    const unsigned volume = radio_service_get_volume();
    const unsigned frame = std::clamp((volume * 2U + 5U) / 10U, 0U, 20U);
    if (frame == volume_frame_)
        return;
    char previous_id[32];
    char current_id[32];
    std::snprintf(previous_id, sizeof(previous_id), "volume_frame_%02u", volume_frame_);
    std::snprintf(current_id, sizeof(current_id), "volume_frame_%02u", frame);
    SetVisible(document_, previous_id, false);
    SetVisible(document_, current_id, true);
    volume_frame_ = frame;
}

void RadioApp::ApplyTunerFrame()
{
    static const char *states[] = {"idle", "previous_focus", "previous_pressed",
                                   "next_focus", "next_pressed"};
    for (const char *state : states)
    {
        char id[40];
        std::snprintf(id, sizeof(id), "tuner_%s", state);
        const bool active =
            (tuner_state_ == 0U && std::strcmp(state, "idle") == 0) ||
            (tuner_state_ == 1U && std::strcmp(state, "previous_focus") == 0) ||
            (tuner_state_ == 2U && std::strcmp(state, "next_focus") == 0);
        SetVisible(document_, id, active);
    }
}

/* --- input ------------------------------------------------------------ */

void RadioApp::HandleInput(const radio_input_event_t &event)
{
    if (event.key == RADIO_INPUT_PAD_CLICK)
    {
        if (event.pressed)
        {
            const unsigned short tx = event.touch_x;
            const unsigned short ty = event.touch_y;
            touch_hold_active_ = event.touch_contact;
            touch_fired_ = false;
            if (touch_hold_active_)
            {
                /* Lock the physical zone at click-down; sliding afterwards
                 * cannot change which preset is recalled or overwritten. */
                touch_zone_ = tx < 640 ? 0 : tx < 1280 ? 1 : 2;
                touch_start_ = radio_input_milliseconds();
                std::fprintf(stderr, "[ProsperoRadio][touch] click-down x=%u y=%u zone=P%u\n",
                             static_cast<unsigned>(tx), static_cast<unsigned>(ty),
                             static_cast<unsigned>(touch_zone_ + 1));
            }
            else
                std::fprintf(stderr, "[ProsperoRadio][touch] click-down without live contact; ignored\n");
        }
        else
        {
            if (touch_hold_active_ && !touch_fired_ && event.touch_contact)
            {
                std::fprintf(stderr, "[ProsperoRadio][touch] short-click recall zone=P%u\n",
                             static_cast<unsigned>(touch_zone_ + 1));
                RecallPreset(touch_zone_);
            }
            else if (touch_fired_)
                std::fprintf(stderr, "[ProsperoRadio][touch] long-click save complete zone=P%u\n",
                             static_cast<unsigned>(touch_zone_ + 1));
            else
                std::fprintf(stderr,
                             "[ProsperoRadio][touch] click released without live contact; gesture cancelled\n");
            touch_hold_active_ = false;
            touch_fired_ = false;
        }
        return;
    }
    if (event.key == RADIO_INPUT_SQUARE)
    {
        if (!event.pressed && aux_delete_hold_active_)
        {
            ReleaseAuxDeleteHold();
            return;
        }
        if (event.pressed && mode_ == Mode::List && list_kind_ == ListKind::Auxiliary)
        {
            BeginAuxDeleteHold();
            return;
        }
    }
    if (!event.pressed)
        return;
    if (aux_delete_hold_active_)
        CancelAuxDeleteHold();
    /* EQ takes the left dial first: it edits the selected band's gain. */
    if (mode_ == Mode::Eq &&
        (event.key == RADIO_INPUT_VOLUME_UP || event.key == RADIO_INPUT_VOLUME_DOWN))
    {
        const int delta = event.key == RADIO_INPUT_VOLUME_UP ? 1 : -1;
        if (eq_sel_ < 12)
            radio_service_eq_set_gain(eq_sel_,
                                      radio_service_eq_gain(eq_sel_) + delta);
        else
        {
            const int channel = eq_sel_ - 12;
            radio_service_eq_set_channel_gain(
                channel, radio_service_eq_channel_gain(channel) + delta);
        }
        SaveEq();
        RefreshEq();
        return;
    }
    if (event.key == RADIO_INPUT_VOLUME_UP || event.key == RADIO_INPUT_VOLUME_DOWN)
    {
        AdjustVolume(event.key == RADIO_INPUT_VOLUME_UP ? 1 : -1);
        return;
    }

    if (disc_device_available_ && mode_ == Mode::Home)
    {
        if (event.key == RADIO_INPUT_UP || event.key == RADIO_INPUT_DOWN)
        {
            const bool returning_to_radio =
                event.key == RADIO_INPUT_DOWN && disc_focus_active_;
            if (returning_to_radio)
            {
                tuned_is_disc_ = false;
                tuned_station_valid_ = false;
                if (pending_play_external_ &&
                    IsDiscStation(disc_stations_, pending_play_station_.uuid))
                {
                    pending_play_external_ = false;
                    pending_play_station_ = radio_station_t{};
                }
                radio_service_status_t status{};
                radio_service_get_status(&status);
                if (IsDiscPlaybackActive(disc_stations_, status))
                {
                    radio_service_stop();
                    std::fprintf(stderr,
                                 "[ProsperoRadio][CD] stop requested on radio focus\n");
                }
            }
            disc_focus_active_ = event.key == RADIO_INPUT_UP;
            disc_animation_tick_ = SDL_GetTicks64();
            UpdateDiscPresentation();
            if (returning_to_radio)
                RefreshHome();
            ApplyButtons();
            return;
        }
        if (disc_focus_active_)
        {
            if (event.key == RADIO_INPUT_OPTIONS)
            {
                radio_service_status_t status{};
                radio_service_get_status(&status);
                if (PlaybackActive(status.playback_state))
                    SetText(document_, "cd-list-status", "STOP PLAYBACK BEFORE SCAN");
                else
                    (void)FetchDiscIndex(true);
                RefreshDiscScreen();
                return;
            }
            if (event.key == RADIO_INPUT_STATION_NEXT ||
                event.key == RADIO_INPUT_STATION_PREVIOUS)
            {
                ChangeDiscSelection(event.key == RADIO_INPUT_STATION_NEXT ? 1 : -1);
                return;
            }
        }
    }

    if (mode_ == Mode::Eq)
    {
        if (event.key == RADIO_INPUT_TRIANGLE || event.key == RADIO_INPUT_CIRCLE)
        {
            mode_ = Mode::Home;
            ApplyButtons();
            RefreshHome();
            return;
        }
        if (event.key == RADIO_INPUT_STATION_NEXT || event.key == RADIO_INPUT_RIGHT)
        {
            eq_sel_ = (eq_sel_ + 1) % 14;
            RefreshEq();
            return;
        }
        if (event.key == RADIO_INPUT_STATION_PREVIOUS || event.key == RADIO_INPUT_LEFT)
        {
            eq_sel_ = (eq_sel_ + 13) % 14;
            RefreshEq();
            return;
        }
        if (event.key == RADIO_INPUT_CROSS)
        {
            eq_preset_ = eq_preset_ < 0 ? 0 : (eq_preset_ + 1) % 4;
            radio_service_eq_preset(eq_preset_);
            SaveEq();
            RefreshEq();
            return;
        }
        return;
    }

    if (mode_ == Mode::Aux || mode_ == Mode::Barrido)
    {
        if (mode_ == Mode::Barrido && event.key == RADIO_INPUT_CROSS &&
            !aux_stations_.empty())
        {
            mode_ = Mode::List;
            list_kind_ = ListKind::Auxiliary;
            list_start_ = list_cursor_ = 0;
            BuildList();
            RefreshList();
            ApplyButtons();
            return;
        }
        if (event.key == RADIO_INPUT_TRIANGLE || event.key == RADIO_INPUT_CIRCLE)
        {
            if (mode_ == Mode::Aux)
            {
                const bool stopped = radio_payload_bridge_aux_stop();
                std::fprintf(stderr, "[ProsperoRadio][AUX] stop on back %s\n",
                             stopped ? "PASS" : "FAIL");
            }
            mode_ = Mode::Home;
            ApplyButtons();
            RefreshHome();
        }
        return;
    }

    switch (mode_)
    {
    case Mode::Search:
        HandleSearchKey(event.key);
        return;
    case Mode::Genres:
        if (event.key == RADIO_INPUT_STATION_NEXT || event.key == RADIO_INPUT_RIGHT ||
            event.key == RADIO_INPUT_DOWN)
        {
            if (genre_total_ && genre_start_ + genre_cursor_ + 1U < genre_total_)
            {
                if (genre_cursor_ + 1U < kListRows)
                    ++genre_cursor_;
                else
                    ++genre_start_;
                RefreshGenres();
            }
            return;
        }
        if (event.key == RADIO_INPUT_STATION_PREVIOUS || event.key == RADIO_INPUT_LEFT ||
            event.key == RADIO_INPUT_UP)
        {
            if (genre_start_ + genre_cursor_ > 0)
            {
                if (genre_cursor_)
                    --genre_cursor_;
                else
                    --genre_start_;
                RefreshGenres();
            }
            return;
        }
        if (event.key == RADIO_INPUT_CROSS)
        {
            const unsigned index = genre_start_ + genre_cursor_;
            if (index < genre_total_)
            {
                CopyString(filter_genre_, sizeof(filter_genre_), genre_facets_[index].value);
                mode_ = Mode::List;
                list_kind_ = ListKind::Radio;
                list_start_ = list_cursor_ = 0;
                BuildList();
                RefreshList();
                ApplyButtons();
            }
            return;
        }
        if (event.key == RADIO_INPUT_CIRCLE || event.key == RADIO_INPUT_TRIANGLE)
        {
            mode_ = Mode::Home;
            ApplyButtons();
            return;
        }
        return;
    case Mode::List:
        if (event.key == RADIO_INPUT_STATION_NEXT || event.key == RADIO_INPUT_DOWN)
        {
            if (list_total_ && list_start_ + list_cursor_ + 1U < list_total_)
            {
                if (list_cursor_ + 1U < kListRows)
                    ++list_cursor_;
                else
                    ++list_start_;
                BuildList();
                RefreshList();
            }
            return;
        }
        if (event.key == RADIO_INPUT_STATION_PREVIOUS || event.key == RADIO_INPUT_UP)
        {
            if (list_start_ + list_cursor_ > 0)
            {
                if (list_cursor_)
                    --list_cursor_;
                else
                    --list_start_;
                BuildList();
                RefreshList();
            }
            return;
        }
        if (event.key == RADIO_INPUT_LEFT || event.key == RADIO_INPUT_RIGHT)
        {
            if (event.key == RADIO_INPUT_RIGHT)
            {
                if (list_kind_ == ListKind::Radio)
                    list_kind_ = ListKind::Favorites;
                else if (list_kind_ == ListKind::Favorites)
                    list_kind_ = ListKind::Auxiliary;
                else if (list_kind_ == ListKind::Auxiliary)
                    list_kind_ = ListKind::Usb;
                else
                    list_kind_ = ListKind::Radio;
            }
            else
            {
                if (list_kind_ == ListKind::Radio)
                    list_kind_ = ListKind::Usb;
                else if (list_kind_ == ListKind::Favorites)
                    list_kind_ = ListKind::Radio;
                else if (list_kind_ == ListKind::Auxiliary)
                    list_kind_ = ListKind::Favorites;
                else
                    list_kind_ = ListKind::Auxiliary;
            }
            list_start_ = list_cursor_ = 0;
            if (list_kind_ == ListKind::Usb)
                (void)FetchUsbIndex(true);
            BuildList();
            RefreshList();
            ApplyButtons();
            return;
        }
        if (event.key == RADIO_INPUT_OPTIONS && list_kind_ == ListKind::Usb)
        {
            (void)FetchUsbIndex(true);
            BuildList();
            RefreshList();
            return;
        }
        if (event.key == RADIO_INPUT_CROSS)
        {
            if (list_indices_[list_cursor_] != kInvalidStation)
            {
                if (list_usb_entries_[list_cursor_])
                {
                    const unsigned index = list_indices_[list_cursor_];
                    if (index < usb_stations_.size())
                        PlayUsbStation(usb_stations_[index]);
                }
                else if (list_aux_entries_[list_cursor_])
                {
                    const auto &source = list_kind_ == ListKind::Favorites ? aux_favorites_ : aux_stations_;
                    const unsigned index = list_indices_[list_cursor_];
                    if (index < source.size())
                        PlayAuxStation(source[index]);
                }
                else
                {
                    tuned_index_ = list_indices_[list_cursor_];
                    PlayIndex(tuned_index_);
                }
                RefreshHome();
            }
            return;
        }
        if (event.key == RADIO_INPUT_SQUARE)
        {
            if (list_usb_entries_[list_cursor_])
            {
                SetText(document_, "list-status", "PISTA USB: SIN FAVORITOS");
            }
            else if (list_aux_entries_[list_cursor_])
            {
                const auto &source = list_kind_ == ListKind::Favorites ? aux_favorites_ : aux_stations_;
                const unsigned index = list_indices_[list_cursor_];
                if (index < source.size())
                {
                    const radio_station_t station = source[index];
                    const bool was_favorite = IsAuxFavorite(station.uuid);
                    if (ToggleAuxFavorite(station))
                        SetText(document_, "home-status", was_favorite ? "AUX FAVORITO ELIMINADO"
                                                                        : "AUX FAVORITO GUARDADO");
                    else
                        SetText(document_, "home-status", "NO SE PUDO GUARDAR FAVORITO AUX");
                }
                BuildList();
                RefreshList();
            }
            else if (list_indices_[list_cursor_] != kInvalidStation)
            {
                radio_service_toggle_favorite(list_indices_[list_cursor_]);
                BuildList();
                RefreshList();
            }
            return;
        }
        if (event.key == RADIO_INPUT_CIRCLE || event.key == RADIO_INPUT_TRIANGLE)
        {
            mode_ = Mode::Home;
            ApplyButtons();
            return;
        }
        return;
    default:
        break;
    }

    // Home mode: D-pad walks the printed buttons, dial tunes.
    if (event.key == RADIO_INPUT_LEFT || event.key == RADIO_INPUT_RIGHT)
    {
        const int next = static_cast<int>(button_focus_) + (event.key == RADIO_INPUT_RIGHT ? 1 : -1);
        button_focus_ = static_cast<unsigned>((next + static_cast<int>(kButtonCount)) %
                                              static_cast<int>(kButtonCount));
        ApplyButtons();
        return;
    }
    if (event.key == RADIO_INPUT_CROSS)
    {
        PressButton(button_focus_);
        return;
    }
    if (event.key == RADIO_INPUT_STATION_NEXT || event.key == RADIO_INPUT_STATION_PREVIOUS)
    {
        TuneStation(event.key == RADIO_INPUT_STATION_NEXT ? 1 : -1);
        return;
    }
    if (event.key == RADIO_INPUT_SQUARE)
    {
        if (tuned_is_disc_)
        {
            SetText(document_, "home-status", "LAS PISTAS DEL CD NO SE GUARDAN EN FAVORITOS");
            return;
        }
        if (tuned_is_usb_)
        {
            SetText(document_, "home-status", "PISTAS USB NO SE GUARDAN EN FAVORITOS AUX");
            return;
        }
        if (tuned_is_aux_)
        {
            const bool was_favorite = IsAuxFavorite(tuned_station_.uuid);
            if (ToggleAuxFavorite(tuned_station_))
                SetText(document_, "home-status", was_favorite ? "AUX FAVORITO ELIMINADO"
                                                                : "AUX FAVORITO GUARDADO");
            else
                SetText(document_, "home-status", "NO SE PUDO GUARDAR FAVORITO AUX");
            return;
        }
        radio_service_toggle_favorite(tuned_index_);
        return;
    }
    if (event.key == RADIO_INPUT_OPTIONS)
    {
        radio_service_refresh();
        return;
    }
}

/* --- poll ------------------------------------------------------------- */

void RadioApp::Poll()
{
    if (poweroff_ticks_ >= 0)
    {
        if (--poweroff_ticks_ <= 0)
        {
            std::fflush(nullptr);
            quit_requested_ = true;
        }
        return;
    }
    if (touch_hold_active_ && !touch_fired_)
    {
        unsigned short tx = 0, ty = 0;
        if (!radio_input_touch(&tx, &ty))
        {
            /* Lifting off before the click ends cancels the pending gesture. */
            touch_hold_active_ = false;
        }
        else if (radio_input_milliseconds() - touch_start_ >= 3000)
        {
            touch_fired_ = true;
            touch_hold_active_ = false;
            StorePreset(touch_zone_);
        }
    }
    UpdateAuxDeleteHold();
    LightbarTick();
    radio_service_status_t status{};
    radio_service_get_status(&status);
    if (!have_last_status_ || status.catalog_generation != last_status_.catalog_generation ||
        status.station_count != last_status_.station_count)
    {
        RebuildFacets();
        genre_total_ = static_cast<unsigned>(genre_facets_.size());
        /* Seed the service query in every mode: HOME reads the tuned station
         * through the same catalog view, and a late sync must not leave it
         * empty ("No stations"). */
        BuildList();
        RefreshGenres();
        RefreshList();
        RefreshHome();
    }
    if ((pending_play_external_ || *pending_play_uuid_) &&
        status.playback_state == RADIO_PLAYBACK_STOPPED)
    {
        if (pending_play_external_)
        {
            radio_station_t pending = pending_play_station_;
            pending_play_external_ = false;
            pending_play_station_ = radio_station_t{};
            if (!radio_service_play_external(&pending))
                SetText(document_, "home-status", "NO SE PUDO ABRIR LA URL M3U");
            radio_service_get_status(&status);
        }
        else
        {
        char pending_uuid[sizeof(pending_play_uuid_)]{};
        CopyString(pending_uuid, sizeof(pending_uuid), pending_play_uuid_);
        pending_play_uuid_[0] = '\0';
        const unsigned pending_index = pending_play_index_;
        pending_play_index_ = InvalidStation;
        radio_station_t station{};
        if (pending_index != InvalidStation &&
            radio_service_get_station(pending_index, &station) &&
            std::strcmp(station.uuid, pending_uuid) == 0)
        {
            radio_service_play(pending_index);
            radio_service_get_status(&status);
        }
        else if (pending_uuid[0] != '\0')
        {
            (void)radio_service_play_uuid(pending_uuid);
            radio_service_get_status(&status);
        }
        }
    }
    if (!have_last_status_ || status.playback_state != last_status_.playback_state ||
        status.playing_index != last_status_.playing_index ||
        status.sample_rate != last_status_.sample_rate ||
        status.channels != last_status_.channels || status.error_code != last_status_.error_code)
    {
        RefreshStatus();
        RefreshHome();
    }
    UpdateEqualizer(status);
    last_status_ = status;
    have_last_status_ = true;

    const unsigned long long now = SDL_GetTicks64();
    if (disc_index_pending_ && now >= disc_index_poll_tick_)
    {
        (void)FetchDiscIndex(false);
        RefreshDiscScreen();
    }
    if (usb_index_pending_ && now >= usb_index_poll_tick_)
        (void)FetchUsbIndex(false);
    if (payload_bridge_was_ready_ && now >= media_index_refresh_tick_)
    {
        radio_station_t playing_station{};
        const bool has_playing_station = PlaybackActive(status.playback_state) &&
                                         radio_service_get_playing_station(&playing_station);
        const bool disc_playing = has_playing_station &&
                                  IsDiscStation(disc_stations_, playing_station.uuid);
        const bool disc_start_pending = pending_play_external_ &&
                                        IsDiscStation(disc_stations_, pending_play_station_.uuid);
        if (!disc_index_pending_ && !disc_playing && !disc_start_pending)
        {
            (void)FetchDiscIndex(true);
            RefreshDiscScreen();
        }
        media_index_refresh_tick_ = now + kDiscIndexRefreshIntervalMs;
    }
    if (payload_bridge_was_ready_ && now >= usb_index_refresh_tick_)
    {
        radio_station_t playing_station{};
        const bool has_playing_station = PlaybackActive(status.playback_state) &&
                                         radio_service_get_playing_station(&playing_station);
        const bool usb_playing = has_playing_station &&
                                 IsUsbStation(usb_stations_, playing_station.uuid);
        const bool usb_start_pending = pending_play_external_ &&
                                       IsUsbStation(usb_stations_, pending_play_station_.uuid);
        if (!usb_index_pending_ && (!usb_playing || !usb_device_available_) &&
            !usb_start_pending)
            (void)FetchUsbIndex(true);
        usb_index_refresh_tick_ = now + kUsbIndexRefreshIntervalMs;
    }
    if (disc_device_available_ && disc_focus_active_ && now >= disc_animation_tick_)
    {
        const bool disc_active = disc_index_pending_ || IsDiscPlaybackActive(disc_stations_, status);
        if (disc_active)
        {
            disc_animation_frame_ = (disc_animation_frame_ + 1U) % kDiscFrameCount;
            ShowDiscFrame(document_, disc_animation_frame_);
        }
        else if (disc_animation_frame_ != 0U)
        {
            disc_animation_frame_ = 0U;
            ShowDiscFrame(document_, disc_animation_frame_);
        }
        disc_animation_tick_ = now + 80U;
        RefreshDiscScreen();
    }
    if (preset_confirm_zone_ >= 0 && radio_input_milliseconds() >= preset_confirm_until_)
    {
        preset_confirm_zone_ = -1;
        ApplyPresetIndicators();
    }
    if (payload_keepalive_tick_ != 0 && now >= payload_keepalive_tick_)
    {
        const bool bridge_ready = radio_payload_bridge_keepalive();
        if (bridge_ready && !payload_bridge_was_ready_)
        {
            std::fprintf(stderr,
                         "[ProsperoRadio][bridge] connection recovered; refreshing AUX/CD/USB\n");
            (void)FetchAuxPlaylist();
            (void)FetchDiscIndex(true);
            (void)FetchUsbIndex(true);
            media_index_refresh_tick_ = now + kDiscIndexRefreshIntervalMs;
            usb_index_refresh_tick_ = now + kUsbIndexRefreshIntervalMs;
        }
        payload_bridge_was_ready_ = bridge_ready;
        payload_keepalive_tick_ = now +
                                  (bridge_ready ? kBridgeKeepaliveIntervalMs
                                                : kBridgeRetryIntervalMs);
    }
}

void RadioApp::UpdateEqualizer(const radio_service_status_t &status)
{
    static const unsigned char wave[16] = {18, 28, 42, 61, 44, 30, 52, 67,
                                           48, 24, 38, 58, 72, 49, 32, 22};
    const bool playing = status.playback_state == RADIO_PLAYBACK_PLAYING;
    const unsigned phase = SDL_GetTicks() / 90U;
    for (unsigned i = 0; i < 5; ++i)
    {
        const int height = playing ? wave[(phase + i * 3U) % 16U] : 8;
        char id[12];
        std::snprintf(id, sizeof(id), "eq-%u", i);
        SetPixelProperty(document_, id, "height", height);
    }
}

/* --- touchpad presets, lightbar pulses and 12-band EQ surface ---------- */

void RadioApp::LoadEq()
{
    std::FILE *file = std::fopen("/download0/radio-eq.txt", "rb");
    if (!file)
        return;

    char format[8]{};
    if (std::fscanf(file, "%7s", format) != 1)
    {
        std::fclose(file);
        return;
    }
    if (std::strcmp(format, "EQ2") == 0)
    {
        int gains[12]{};
        int left_gain = 0;
        int right_gain = 0;
        bool complete = true;
        for (int band = 0; band < 12; ++band)
            complete = complete && std::fscanf(file, "%d", &gains[band]) == 1;
        complete = complete && std::fscanf(file, "%d %d", &left_gain, &right_gain) == 2;
        if (complete)
        {
            for (int band = 0; band < 12; ++band)
                radio_service_eq_set_gain(band, gains[band]);
            radio_service_eq_set_channel_gain(0, left_gain);
            radio_service_eq_set_channel_gain(1, right_gain);
        }
    }
    else
    {
        /* Migrate the former five-band file onto its matching 60/250/1k/4k/12k
         * centers; the seven added bands start flat. */
        static const int legacy_band[5] = {0, 2, 4, 8, 11};
        int legacy_gain[5]{};
        bool complete = std::fseek(file, 0, SEEK_SET) == 0;
        for (int band = 0; band < 5; ++band)
            complete = complete && std::fscanf(file, "%d", &legacy_gain[band]) == 1;
        if (complete)
        {
            for (int band = 0; band < 12; ++band)
                radio_service_eq_set_gain(band, 0);
            for (int band = 0; band < 5; ++band)
                radio_service_eq_set_gain(legacy_band[band], legacy_gain[band]);
        }
    }
    std::fclose(file);
}

void RadioApp::SaveEq()
{
    constexpr char path[] = "/download0/radio-eq.txt";
    constexpr char temporary[] = "/download0/radio-eq.txt.tmp";
    std::FILE *file = std::fopen(temporary, "wb");
    if (!file)
        return;
    bool wrote = std::fputs("EQ2", file) >= 0;
    for (int band = 0; band < 12; ++band)
        wrote = wrote && std::fprintf(file, " %d", radio_service_eq_gain(band)) >= 0;
    wrote = wrote && std::fprintf(file, " %d %d\n", radio_service_eq_channel_gain(0),
                                  radio_service_eq_channel_gain(1)) >= 0;
    wrote = wrote && std::fflush(file) == 0;
    const bool closed = std::fclose(file) == 0;
    if (!wrote || !closed || std::rename(temporary, path) != 0)
    {
        std::remove(temporary);
        return;
    }
    (void)radio_payload_bridge_push(RADIO_PAYLOAD_EQ);
}

bool RadioApp::IsAuxFavorite(const char *uuid) const
{
    if (!uuid || !*uuid)
        return false;
    return std::any_of(aux_favorites_.begin(), aux_favorites_.end(),
                       [uuid](const radio_station_t &station)
                       { return std::strcmp(station.uuid, uuid) == 0; });
}

void RadioApp::LoadAuxFavorites()
{
    aux_favorites_.clear();
    std::FILE *file = std::fopen("/download0/radio-aux-favorites.bin", "rb");
    if (!file)
        return;
    AuxFavoriteFileHeader header{};
    std::vector<radio_station_t> loaded;
    bool valid = std::fread(&header, sizeof(header), 1, file) == 1 &&
                 std::memcmp(header.magic, kAuxFavoriteFileMagic, sizeof(header.magic)) == 0 &&
                 header.version == kAuxFavoriteFileVersion && header.count <= kAuxMaxStations;
    if (valid)
    {
        loaded.reserve(header.count);
        for (std::uint32_t index = 0; index < header.count; ++index)
        {
            radio_station_t station{};
            if (std::fread(&station, sizeof(station), 1, file) != 1 ||
                std::memchr(station.uuid, '\0', sizeof(station.uuid)) == nullptr ||
                !station.uuid[0] || std::memchr(station.name, '\0', sizeof(station.name)) == nullptr ||
                std::memchr(station.url, '\0', sizeof(station.url)) == nullptr ||
                !IsHttpUrl(station.url))
            {
                valid = false;
                break;
            }
            loaded.push_back(station);
        }
        if (valid && std::fgetc(file) != EOF)
            valid = false;
    }
    std::fclose(file);
    if (!valid)
    {
        std::fprintf(stderr, "[ProsperoRadio][AUX] favorites file rejected; source preserved\n");
        return;
    }
    aux_favorites_.swap(loaded);
    std::fprintf(stderr, "[ProsperoRadio][AUX] favorites loaded=%u\n",
                 static_cast<unsigned>(aux_favorites_.size()));
}

bool RadioApp::SaveAuxFavorites()
{
    if (aux_favorites_.size() > kAuxMaxStations)
        return false;
    AuxFavoriteFileHeader header{};
    std::memcpy(header.magic, kAuxFavoriteFileMagic, sizeof(header.magic));
    header.version = kAuxFavoriteFileVersion;
    header.count = static_cast<std::uint32_t>(aux_favorites_.size());
    constexpr char path[] = "/download0/radio-aux-favorites.bin";
    constexpr char temporary[] = "/download0/radio-aux-favorites.bin.tmp";
    std::FILE *file = std::fopen(temporary, "wb");
    if (!file)
        return false;
    bool wrote = std::fwrite(&header, sizeof(header), 1, file) == 1;
    for (const radio_station_t &station : aux_favorites_)
        wrote = wrote && std::fwrite(&station, sizeof(station), 1, file) == 1;
    wrote = wrote && std::fflush(file) == 0;
    const bool closed = std::fclose(file) == 0;
    if (!wrote || !closed || std::rename(temporary, path) != 0)
    {
        std::remove(temporary);
        return false;
    }
    return radio_payload_bridge_push(RADIO_PAYLOAD_AUXFAVORITES);
}

bool RadioApp::ToggleAuxFavorite(const radio_station_t &station)
{
    if (!station.uuid[0] || !IsHttpUrl(station.url))
        return false;
    const auto found = std::find_if(aux_favorites_.begin(), aux_favorites_.end(),
                                    [&station](const radio_station_t &saved)
                                    { return std::strcmp(saved.uuid, station.uuid) == 0; });
    if (found != aux_favorites_.end())
        aux_favorites_.erase(found);
    else
    {
        if (aux_favorites_.size() >= kAuxMaxStations)
            return false;
        aux_favorites_.push_back(station);
    }
    return SaveAuxFavorites();
}

bool RadioApp::SaveAuxPlaylist(const std::vector<radio_station_t> &stations,
                               const char *excluded_uuid)
{
    const bool exclude_station = excluded_uuid && *excluded_uuid;
    if (exclude_station &&
        std::find_if(stations.begin(), stations.end(),
                     [excluded_uuid](const radio_station_t &station)
                     { return std::strcmp(station.uuid, excluded_uuid) == 0; }) == stations.end())
        return false;
    const std::size_t output_count = stations.size() - (exclude_station ? 1U : 0U);
    if (stations.size() > kAuxMaxStations || output_count > kAuxMaxStations)
        return false;
    std::string document = "#EXTM3U\n";
    document.reserve(std::min<std::size_t>(RADIO_AUX_PLAYLIST_MAX_BYTES,
                                           output_count * 256U + 8U));
    for (const radio_station_t &station : stations)
    {
        if (exclude_station && std::strcmp(station.uuid, excluded_uuid) == 0)
            continue;
        if (!IsHttpUrl(station.url))
            return false;
        std::string name(station.name);
        std::string group(station.tags[0] ? station.tags : "AUX");
        for (char &character : name)
            if (character == '\r' || character == '\n' || character == '"')
                character = '\'';
        for (char &character : group)
            if (character == '\r' || character == '\n' || character == '"')
                character = ' ';
        char entry[1024];
        const int length = std::snprintf(entry, sizeof(entry),
                                         "#EXTINF:-1 group-title=\"%s\",%s\n%s\n",
                                         group.c_str(), name.c_str(), station.url);
        if (length <= 0 || static_cast<std::size_t>(length) >= sizeof(entry) ||
            static_cast<std::size_t>(length) > RADIO_AUX_PLAYLIST_MAX_BYTES - document.size())
            return false;
        document.append(entry, static_cast<std::size_t>(length));
    }
    const bool stored = radio_payload_bridge_store_aux_buffer(
        document.data(), document.size(), RADIO_AUX_FORMAT_M3U);
    if (stored)
        radio_service_aux_stations_set(static_cast<int>(output_count));
    std::fprintf(stderr, "[ProsperoRadio][AUX] edited list stored=%u entries=%u bytes=%u\n",
                 static_cast<unsigned>(stored), static_cast<unsigned>(output_count),
                 static_cast<unsigned>(document.size()));
    return stored;
}

bool RadioApp::FetchAuxPlaylist()
{
    std::vector<unsigned char> document(RADIO_AUX_PLAYLIST_MAX_BYTES +
                                        RADIO_AUX_PLAYLIST_ENVELOPE_BYTES);
    std::size_t size = 0U;
    unsigned char format = RADIO_AUX_FORMAT_AUTO;
    bool not_found = false;
    if (!radio_payload_bridge_fetch_aux_buffer(document.data(), document.size(), &size,
                                               &format, &not_found))
    {
        if (not_found)
        {
            aux_stations_.clear();
            radio_service_aux_stations_set(0);
        }
        return false;
    }
    (void)ScanAuxPlaylist(document.data(), size, format);
    return true;
}

bool RadioApp::DeleteAuxStation(const char *uuid)
{
    if (!uuid || !*uuid)
        return false;
    const auto found = std::find_if(aux_stations_.begin(), aux_stations_.end(),
                                    [uuid](const radio_station_t &station)
                                    { return std::strcmp(station.uuid, uuid) == 0; });
    if (found == aux_stations_.end())
        return false;
    if (!SaveAuxPlaylist(aux_stations_, uuid))
    {
        SetText(document_, "home-status", "NO SE PUDO GUARDAR LA LISTA AUX");
        std::fprintf(stderr, "[ProsperoRadio][AUX] delete failed station=%s stage=persist\n", uuid);
        return false;
    }
    aux_stations_.erase(found);
    BuildList();
    RefreshList();
    SetText(document_, "home-status", "EMISORA ELIMINADA DE AUX");
    std::fprintf(stderr, "[ProsperoRadio][AUX] station removed uuid=%s remaining=%u\n", uuid,
                 static_cast<unsigned>(aux_stations_.size()));
    return true;
}

void RadioApp::BeginAuxDeleteHold()
{
    if (list_kind_ != ListKind::Auxiliary || list_cursor_ >= kListRows ||
        list_indices_[list_cursor_] == kInvalidStation || !list_aux_entries_[list_cursor_])
        return;
    const unsigned index = list_indices_[list_cursor_];
    if (index >= aux_stations_.size())
        return;
    aux_delete_hold_active_ = true;
    CopyString(aux_delete_hold_uuid_, sizeof(aux_delete_hold_uuid_),
               aux_stations_[index].uuid);
    aux_delete_hold_start_ = radio_input_milliseconds();
    aux_delete_display_tenth_ = ~0U;
    std::fprintf(stderr, "[ProsperoRadio][AUX] delete-hold start uuid=%s duration_ms=%llu\n",
                 aux_delete_hold_uuid_, kAuxDeleteHoldMs);
    RefreshList();
}

void RadioApp::CancelAuxDeleteHold()
{
    if (!aux_delete_hold_active_)
        return;
    aux_delete_hold_active_ = false;
    aux_delete_hold_uuid_[0] = '\0';
    aux_delete_display_tenth_ = ~0U;
    if (mode_ == Mode::List)
        RefreshList();
}

void RadioApp::ReleaseAuxDeleteHold()
{
    if (!aux_delete_hold_active_)
        return;
    const unsigned long long elapsed = radio_input_milliseconds() - aux_delete_hold_start_;
    char uuid[sizeof(aux_delete_hold_uuid_)];
    CopyString(uuid, sizeof(uuid), aux_delete_hold_uuid_);
    aux_delete_hold_active_ = false;
    aux_delete_hold_uuid_[0] = '\0';
    aux_delete_display_tenth_ = ~0U;
    if (elapsed >= kAuxDeleteHoldMs)
    {
        if (!DeleteAuxStation(uuid))
            RefreshList();
        return;
    }

    const auto found = std::find_if(aux_stations_.begin(), aux_stations_.end(),
                                    [uuid](const radio_station_t &station)
                                    { return std::strcmp(station.uuid, uuid) == 0; });
    if (found != aux_stations_.end())
    {
        const radio_station_t station = *found;
        const bool was_favorite = IsAuxFavorite(station.uuid);
        if (ToggleAuxFavorite(station))
            SetText(document_, "home-status", was_favorite ? "AUX FAVORITO ELIMINADO"
                                                            : "AUX FAVORITO GUARDADO");
        else
            SetText(document_, "home-status", "NO SE PUDO GUARDAR FAVORITO AUX");
    }
    RefreshList();
}

void RadioApp::UpdateAuxDeleteHold()
{
    if (!aux_delete_hold_active_)
        return;
    const unsigned long long elapsed = radio_input_milliseconds() - aux_delete_hold_start_;
    if (elapsed >= kAuxDeleteHoldMs)
    {
        char uuid[sizeof(aux_delete_hold_uuid_)];
        CopyString(uuid, sizeof(uuid), aux_delete_hold_uuid_);
        aux_delete_hold_active_ = false;
        aux_delete_hold_uuid_[0] = '\0';
        aux_delete_display_tenth_ = ~0U;
        if (!DeleteAuxStation(uuid))
            RefreshList();
        return;
    }
    const unsigned long long remaining = kAuxDeleteHoldMs - elapsed;
    const unsigned display_tenth = static_cast<unsigned>((remaining + 99ULL) / 100ULL);
    if (display_tenth != aux_delete_display_tenth_)
    {
        aux_delete_display_tenth_ = display_tenth;
        if (mode_ == Mode::List && list_kind_ == ListKind::Auxiliary)
            RefreshList();
    }
}

void RadioApp::LoadPresets()
{
    presets_[0] = presets_[1] = presets_[2] = -1;
    std::memset(preset_uuids_, 0, sizeof(preset_uuids_));
    std::memset(preset_saved_, 0, sizeof(preset_saved_));
    std::memset(preset_external_, 0, sizeof(preset_external_));
    std::memset(preset_external_station_, 0, sizeof(preset_external_station_));
    std::FILE *file = std::fopen("/download0/radio-presets.bin", "rb");
    if (file)
    {
        PresetFileV3 saved{};
        const bool loaded_v3 = std::fread(&saved, sizeof(saved), 1, file) == 1 &&
                               std::memcmp(saved.magic, kPresetFileMagicV3, sizeof(saved.magic)) == 0 &&
                               saved.version == kPresetFileVersion;
        if (loaded_v3)
        {
            for (int zone = 0; zone < 3; ++zone)
            {
                if (saved.external[zone] > 1U || saved.indices[zone] < -1 ||
                    std::memchr(saved.uuids[zone], '\0', sizeof(saved.uuids[zone])) == nullptr)
                    continue;
                if (saved.external[zone])
                {
                    const radio_station_t &station = saved.external_stations[zone];
                    if (std::memchr(station.uuid, '\0', sizeof(station.uuid)) == nullptr ||
                        std::memchr(station.url, '\0', sizeof(station.url)) == nullptr ||
                        !station.uuid[0] || !IsHttpUrl(station.url))
                        continue;
                    preset_external_[zone] = true;
                    preset_external_station_[zone] = station;
                    presets_[zone] = -1;
                    CopyString(preset_uuids_[zone], sizeof(preset_uuids_[zone]), station.uuid);
                    preset_saved_[zone] = true;
                }
                else if (saved.indices[zone] >= 0 && saved.uuids[zone][0])
                {
                    presets_[zone] = static_cast<int>(saved.indices[zone]);
                    CopyString(preset_uuids_[zone], sizeof(preset_uuids_[zone]), saved.uuids[zone]);
                    preset_saved_[zone] = true;
                }
            }
        }
        else
        {
            std::rewind(file);
            PresetFileV2 old{};
            const bool loaded_v2 = std::fread(&old, sizeof(old), 1, file) == 1 &&
                                   std::memcmp(old.magic, kPresetFileMagic, sizeof(old.magic)) == 0 &&
                                   old.version == 2U;
            if (loaded_v2)
            {
                for (int zone = 0; zone < 3; ++zone)
                {
                    if (old.indices[zone] < 0 ||
                        std::memchr(old.uuids[zone], '\0', sizeof(old.uuids[zone])) == nullptr ||
                        !old.uuids[zone][0])
                        continue;
                    presets_[zone] = static_cast<int>(old.indices[zone]);
                    CopyString(preset_uuids_[zone], sizeof(preset_uuids_[zone]), old.uuids[zone]);
                    preset_saved_[zone] = true;
                }
            }
            else
            {
                std::rewind(file);
                int legacy_indices[3] = {-1, -1, -1};
                if (std::fread(legacy_indices, sizeof(legacy_indices), 1, file) == 1)
                    for (int zone = 0; zone < 3; ++zone)
                        if (legacy_indices[zone] >= 0)
                            presets_[zone] = legacy_indices[zone];
            }
        }
        std::fclose(file);
    }

    bool migrated = false;
    for (int zone = 0; zone < 3; ++zone)
    {
        if (preset_saved_[zone] || presets_[zone] < 0)
            continue;
        radio_station_t station{};
        if (radio_service_get_station(static_cast<unsigned>(presets_[zone]), &station) &&
            station.uuid[0] != '\0')
        {
            CopyString(preset_uuids_[zone], sizeof(preset_uuids_[zone]), station.uuid);
            preset_saved_[zone] = true;
            migrated = true;
        }
    }
    if (migrated)
        SavePresets();
}

bool RadioApp::SavePresets()
{
    PresetFileV3 saved{};
    std::memcpy(saved.magic, kPresetFileMagicV3, sizeof(saved.magic));
    saved.version = kPresetFileVersion;
    for (int zone = 0; zone < 3; ++zone)
    {
        saved.indices[zone] = static_cast<std::int32_t>(presets_[zone]);
        CopyString(saved.uuids[zone], sizeof(saved.uuids[zone]), preset_uuids_[zone]);
        saved.external[zone] = preset_external_[zone] ? 1U : 0U;
        if (preset_external_[zone])
            saved.external_stations[zone] = preset_external_station_[zone];
    }
    constexpr char path[] = "/download0/radio-presets.bin";
    constexpr char temporary[] = "/download0/radio-presets.bin.tmp";
    std::FILE *file = std::fopen(temporary, "wb");
    if (!file)
        return false;
    const bool wrote = std::fwrite(&saved, sizeof(saved), 1, file) == 1 &&
                       std::fflush(file) == 0;
    const bool closed = std::fclose(file) == 0;
    if (!wrote || !closed || std::rename(temporary, path) != 0)
    {
        std::remove(temporary);
        return false;
    }
    return radio_payload_bridge_push(RADIO_PAYLOAD_PRESETS);
}

void RadioApp::StorePreset(int zone)
{
    if (zone < 0 || zone > 2)
        return;
    if (tuned_is_disc_)
    {
        SetText(document_, "home-status", "LAS PISTAS DEL CD NO SE GUARDAN EN PRESETS");
        return;
    }
    radio_station_t station{};
    if (tuned_is_aux_ || tuned_is_usb_)
        station = tuned_station_;
    else if (!radio_service_get_station(tuned_index_, &station))
    {
        SetText(document_, "home-status", "NO SE PUEDE GUARDAR ESTA EMISORA");
        return;
    }
    if (station.uuid[0] == '\0' || !IsHttpUrl(station.url))
    {
        SetText(document_, "home-status", "NO SE PUEDE GUARDAR ESTA EMISORA");
        return;
    }
    const bool external = tuned_is_aux_ || tuned_is_usb_;
    preset_external_[zone] = external;
    preset_external_station_[zone] = external ? station : radio_station_t{};
    presets_[zone] = external ? -1 : static_cast<int>(tuned_index_);
    CopyString(preset_uuids_[zone], sizeof(preset_uuids_[zone]), station.uuid);
    preset_saved_[zone] = true;
    tuned_station_ = station;
    tuned_station_valid_ = true;
    preset_active_ = zone;
    const bool persisted = SavePresets();
    StartPresetFeedback(zone);
    preset_confirm_zone_ = zone;
    preset_confirm_until_ = radio_input_milliseconds() + 1200U;
    ApplyPresetIndicators();
    const char *slot = zone == 0 ? "P1" : zone == 1 ? "P2" : "P3";
    char status_text[72];
    std::snprintf(status_text, sizeof(status_text), "%s %s", slot,
                  persisted ? "MEMORIZADA" : "GUARDADA LOCAL; ERROR EN /DATA");
    SetText(document_, "home-status", status_text);
}

void RadioApp::RecallPreset(int zone)
{
    if (zone < 0 || zone > 2)
        return;
    if (!preset_saved_[zone])
    {
        SetText(document_, "home-status",
                zone == 0 ? "P1 VACIA" : zone == 1 ? "P2 VACIA" : "P3 VACIA");
        return;
    }
    radio_station_t station{};
    const bool external = preset_external_[zone];
    if (external)
    {
        station = preset_external_station_[zone];
        if (!station.uuid[0] || !IsHttpUrl(station.url))
        {
            SetText(document_, "home-status", "EMISORA PRESET NO DISPONIBLE");
            return;
        }
    }
    else if (preset_uuids_[zone][0] == '\0')
    {
        if (presets_[zone] < 0 ||
            !radio_service_get_station(static_cast<unsigned>(presets_[zone]), &station) ||
            station.uuid[0] == '\0')
        {
            SetText(document_, "home-status", "EMISORA PRESET NO DISPONIBLE");
            return;
        }
        CopyString(preset_uuids_[zone], sizeof(preset_uuids_[zone]), station.uuid);
        preset_saved_[zone] = true;
        (void)SavePresets();
    }
    else if (!radio_service_get_station_by_uuid(preset_uuids_[zone], &station))
    {
        SetText(document_, "home-status", "EMISORA PRESET NO DISPONIBLE");
        return;
    }
    preset_active_ = zone;
    const bool usb_station = std::strncmp(station.uuid, "usb-audio-", 10U) == 0;
    tuned_is_aux_ = external && !usb_station;
    tuned_is_disc_ = false;
    tuned_is_usb_ = external && usb_station;
    if (disc_device_available_ && disc_focus_active_ &&
        !IsDiscStation(disc_stations_, station.uuid))
    {
        disc_focus_active_ = false;
        disc_animation_frame_ = 0U;
        disc_animation_tick_ = SDL_GetTicks64();
        UpdateDiscPresentation();
        std::fprintf(stderr,
                     "[ProsperoRadio][CD] display slept for non-disc preset playback\n");
    }
    if (!external && presets_[zone] >= 0)
        tuned_index_ = static_cast<unsigned>(presets_[zone]);
    tuned_station_ = station;
    tuned_station_valid_ = true;
    StartPresetFeedback(zone);
    ApplyPresetIndicators();
    radio_service_status_t status{};
    radio_service_get_status(&status);
    radio_station_t playing{};
    if (PlaybackActive(status.playback_state))
    {
        if (radio_service_get_playing_station(&playing) &&
            std::strcmp(playing.uuid, preset_uuids_[zone]) == 0)
        {
            RefreshHome();
            return;
        }
        pending_play_index_ = InvalidStation;
        if (external)
        {
            pending_play_station_ = station;
            pending_play_external_ = true;
            pending_play_uuid_[0] = '\0';
        }
        else
        {
            CopyString(pending_play_uuid_, sizeof(pending_play_uuid_), preset_uuids_[zone]);
            pending_play_external_ = false;
        }
        radio_service_stop();
    }
    else if (!(external ? radio_service_play_external(&station)
                        : radio_service_play_uuid(preset_uuids_[zone])))
        SetText(document_, "home-status", "NO SE PUDO INICIAR EL PRESET");
    RefreshHome();
}

void RadioApp::StartPresetFeedback(int zone)
{
    if (zone < 0 || zone > 2)
        return;
    lb_pulses_left_ = zone + 1;
    lb_feedback_active_ = true;
    lb_next_transition_ = radio_input_milliseconds();
    if (lb_on_)
    {
        radio_input_lightbar(0, 0, 0);
        lb_on_ = false;
    }
    std::fprintf(stderr, "[ProsperoRadio][preset] feedback start P%d pulses=%d\n",
                 zone + 1, lb_pulses_left_);
}

void RadioApp::ApplyPresetIndicators()
{
    for (int zone = 0; zone < 3; ++zone)
    {
        char id[16];
        std::snprintf(id, sizeof(id), "chip-p%d", zone);
        const bool saved = preset_saved_[zone];
        SetClass(document_, id, "chip-off", !saved);
        SetClass(document_, id, "chip-saved", saved);
        SetClass(document_, id, "chip-active", saved && preset_active_ == zone);
        SetClass(document_, id, "chip-confirmed", saved && preset_confirm_zone_ == zone);
    }
}

void RadioApp::LightbarTick()
{
    const unsigned long long now = radio_input_milliseconds();
    const bool holding = touch_hold_active_ && !touch_fired_;
    bool on = false;
    if (holding)
        on = (now / 180U) % 2U == 0U;
    else if (lb_feedback_active_)
    {
        if (now >= lb_next_transition_)
        {
            if (lb_on_)
            {
                on = false;
                --lb_pulses_left_;
                if (lb_pulses_left_ <= 0)
                {
                    lb_pulses_left_ = 0;
                    lb_feedback_active_ = false;
                }
            }
            else
                on = true;
            lb_next_transition_ = now + 180U;
        }
        else
            on = lb_on_;
    }
    else
        on = preset_active_ >= 0;
    if (on != lb_on_)
    {
        lb_on_ = on;
        radio_input_lightbar(on ? 255 : 0, on ? 128 : 0, 0);
    }
}

void RadioApp::RefreshEq()
{
    const auto refresh_control = [this](int control, int gain)
    {
        char id[16];
        std::snprintf(id, sizeof(id), "eqband-%d", control);
        SetClass(document_, id, "selected", control == eq_sel_);

        std::snprintf(id, sizeof(id), "eqgain-%d", control);
        char text[32];
        std::snprintf(text, sizeof(text), "%s%ddB", gain > 0 ? "+" : "", gain);
        SetText(document_, id, text);

        const int bounded = std::max(-12, std::min(12, gain));
        const int height = std::abs(bounded) * 4;
        const int top = bounded > 0 ? 50 - height : bounded < 0 ? 50 : 49;
        std::snprintf(id, sizeof(id), "eqfill-%d", control);
        SetPixelProperty(document_, id, "top", top);
        SetPixelProperty(document_, id, "height", bounded == 0 ? 2 : height);
    };

    for (int band = 0; band < 12; ++band)
        refresh_control(band, radio_service_eq_gain(band));
    refresh_control(12, radio_service_eq_channel_gain(0));
    refresh_control(13, radio_service_eq_channel_gain(1));
}

void RadioApp::RefreshAuxPanel()
{
    SetText(document_, "aux-url", radio_service_aux_running()
                                      ? "AUX ACTIVO - PUERTO 7000"
                                      : "RED NO DISPONIBLE");
    const int stations = radio_service_aux_stations();
    char note[128];
    if (stations >= 0)
        std::snprintf(note, sizeof(note), "AUX %d EMISORAS | ENVIA M3U, PLS, XSPF O ASX",
                      stations);
    else
        CopyString(note, sizeof(note), "SUBE M3U, M3U8, PLS, XSPF O ASX DESDE MOVIL O PC");
    SetText(document_, "aux-note", note);
}

unsigned RadioApp::ScanAuxPlaylist(const unsigned char *data, std::size_t size,
                                   unsigned char format_hint)
{
    constexpr std::size_t kAuxPlaylistMaxBytes = RADIO_AUX_PLAYLIST_MAX_BYTES;
    if (data == nullptr || size == 0U || size > kAuxPlaylistMaxBytes)
    {
        std::fprintf(stderr,
                     "[ProsperoRadio][AUX] scan rejected reason=buffer-or-size bytes=%llu; "
                     "previous list kept\n",
                     (unsigned long long)size);
        return static_cast<unsigned>(aux_stations_.size());
    }
    std::string_view document(reinterpret_cast<const char *>(data), size);
    if (document.find('\0') != std::string_view::npos)
    {
        std::fprintf(stderr,
                     "[ProsperoRadio][AUX] scan rejected reason=embedded-nul bytes=%llu; "
                     "previous list kept\n",
                     (unsigned long long)size);
        return static_cast<unsigned>(aux_stations_.size());
    }
    if (format_hint > RADIO_AUX_FORMAT_ASX)
        format_hint = RADIO_AUX_FORMAT_AUTO;

    if (document.size() >= 3U &&
        static_cast<unsigned char>(document[0]) == 0xefU &&
        static_cast<unsigned char>(document[1]) == 0xbbU &&
        static_cast<unsigned char>(document[2]) == 0xbfU)
        document.remove_prefix(3U);
    if (format_hint == RADIO_AUX_FORMAT_M3U8 && !IsValidUtf8(document))
    {
        std::fprintf(stderr,
                     "[ProsperoRadio][AUX] scan rejected format=m3u8 reason=invalid-utf8; "
                     "previous list kept\n");
        return static_cast<unsigned>(aux_stations_.size());
    }

    std::vector<radio_station_t> parsed;
    parsed.reserve(128U);
    const bool is_hls_manifest = FindCi(document, "#EXT-X-") != std::string::npos;
    bool format_recognized = false;
    const char *format = "unknown";
    if (is_hls_manifest)
    {
        std::fprintf(stderr,
                     "[ProsperoRadio][AUX] scan rejected format=hls-manifest; "
                     "this endpoint is not a station-list file\n");
        return static_cast<unsigned>(aux_stations_.size());
    }
    const bool content_pls = FindCi(document, "[playlist]") != std::string::npos ||
                             FindCi(document, "file1=") != std::string::npos;
    const bool content_xspf = FindCi(document, "xspf.org/ns") != std::string::npos ||
                              (FindCi(document, "<tracklist") != std::string::npos &&
                               FindCi(document, "<location") != std::string::npos);
    const bool content_asx = FindCi(document, "<asx") != std::string::npos;
    const bool has_m3u_header = FindCi(document, "#EXTM3U") != std::string::npos;
    bool content_m3u = has_m3u_header || FindCi(document, "#EXTINF:") != std::string::npos;
    for (std::size_t position = 0U; !content_m3u && position < document.size();)
    {
        const std::size_t end = document.find('\n', position);
        const std::string_view line = TrimView(document.substr(
            position, (end == std::string::npos ? document.size() : end) - position));
        content_m3u = line.size() >= 7U && strncasecmp(line.data(), "http://", 7U) == 0;
        content_m3u = content_m3u ||
                      (line.size() >= 8U && strncasecmp(line.data(), "https://", 8U) == 0);
        position = end == std::string::npos ? document.size() : end + 1U;
    }
    const bool use_pls = content_pls ||
                         (format_hint == RADIO_AUX_FORMAT_PLS && !content_xspf && !content_asx &&
                          !content_m3u);
    const bool use_xspf = content_xspf ||
                          (format_hint == RADIO_AUX_FORMAT_XSPF && !content_pls && !content_asx &&
                           !content_m3u);
    const bool use_asx = content_asx ||
                         (format_hint == RADIO_AUX_FORMAT_ASX && !content_pls && !content_xspf &&
                          !content_m3u);
    if (use_pls)
    {
        format = "pls";
        format_recognized = true;
        struct PlsRecord
        {
            std::string url;
            std::string title;
        };
        std::map<unsigned, PlsRecord> entries;
        std::size_t position = 0U;
        while (position < document.size())
        {
            const std::size_t end = document.find('\n', position);
            const std::string_view line = TrimView(std::string_view(document).substr(
                position, (end == std::string::npos ? document.size() : end) - position));
            const std::size_t equals = line.find('=');
            if (equals != std::string_view::npos)
            {
                const std::string_view key = TrimView(line.substr(0U, equals));
                const std::string_view value = TrimView(line.substr(equals + 1U));
                const bool is_file = key.size() > 4U &&
                                     std::tolower((unsigned char)key[0]) == 'f' &&
                                     std::tolower((unsigned char)key[1]) == 'i' &&
                                     std::tolower((unsigned char)key[2]) == 'l' &&
                                     std::tolower((unsigned char)key[3]) == 'e';
                const bool is_title = key.size() > 5U &&
                                      std::tolower((unsigned char)key[0]) == 't' &&
                                      std::tolower((unsigned char)key[1]) == 'i' &&
                                      std::tolower((unsigned char)key[2]) == 't' &&
                                      std::tolower((unsigned char)key[3]) == 'l' &&
                                      std::tolower((unsigned char)key[4]) == 'e';
                const std::size_t prefix = is_file ? 4U : is_title ? 5U : key.size();
                if ((is_file || is_title) && prefix < key.size())
                {
                    unsigned index = 0U;
                    bool digits = true;
                    for (std::size_t at = prefix; at < key.size(); ++at)
                    {
                        if (key[at] < '0' || key[at] > '9' ||
                            index > (UINT_MAX - (unsigned)(key[at] - '0')) / 10U)
                        {
                            digits = false;
                            break;
                        }
                        index = index * 10U + (unsigned)(key[at] - '0');
                    }
                    if (digits && index != 0U)
                    {
                        PlsRecord &entry = entries[index];
                        (is_file ? entry.url : entry.title).assign(value.data(), value.size());
                    }
                }
            }
            position = end == std::string::npos ? document.size() : end + 1U;
        }
        for (const auto &item : entries)
        {
            if (item.second.url.empty())
                continue;
            std::string extinf = "#EXTINF:-1," + item.second.title;
            AddAuxStation(parsed, item.second.url, std::move(extinf));
        }
    }
    else if (use_xspf)
    {
        format = "xspf";
        format_recognized = true;
        std::size_t position = 0U;
        while (parsed.size() < kAuxMaxStations)
        {
            const std::size_t open = FindCi(document, "<track", position);
            if (open == std::string::npos)
                break;
            const std::size_t tag_end = document.find('>', open + 6U);
            if (tag_end == std::string::npos)
                break;
            if (tag_end > open + 6U &&
                !std::isspace((unsigned char)document[open + 6U]) &&
                document[open + 6U] != '>')
            {
                position = tag_end + 1U;
                continue;
            }
            const std::size_t close = FindCi(document, "</track>", tag_end + 1U);
            if (close == std::string::npos)
                break;
            const std::string_view track(document.data() + open, close + 8U - open);
            std::string url;
            std::string title;
            if (XmlTagContent(track, "location", url))
            {
                (void)XmlTagContent(track, "title", title);
                AddAuxStation(parsed, url, "#EXTINF:-1," + title);
            }
            position = close + 8U;
        }
    }
    else if (use_asx)
    {
        format = "asx";
        format_recognized = true;
        std::size_t position = 0U;
        while (parsed.size() < kAuxMaxStations)
        {
            const std::size_t open = FindCi(document, "<entry", position);
            if (open == std::string::npos)
                break;
            const std::size_t tag_end = document.find('>', open + 6U);
            if (tag_end == std::string::npos)
                break;
            if (tag_end > open + 6U &&
                !std::isspace((unsigned char)document[open + 6U]) &&
                document[open + 6U] != '>')
            {
                position = tag_end + 1U;
                continue;
            }
            const std::size_t close = FindCi(document, "</entry>", tag_end + 1U);
            if (close == std::string::npos)
                break;
            const std::string_view entry(document.data() + open, close + 8U - open);
            const std::size_t ref = FindCi(entry, "<ref");
            std::string url;
            std::string title;
            if (ref != std::string_view::npos)
            {
                const std::size_t ref_end = entry.find('>', ref + 4U);
                if (ref_end != std::string_view::npos)
                    (void)XmlAttribute(entry.substr(ref, ref_end - ref + 1U), "href", url);
            }
            if (!url.empty())
            {
                (void)XmlTagContent(entry, "title", title);
                AddAuxStation(parsed, url, "#EXTINF:-1," + title);
            }
            position = close + 8U;
        }
    }
    else
    {
        format = format_hint == RADIO_AUX_FORMAT_M3U8 ? "m3u8"
                 : format_hint == RADIO_AUX_FORMAT_M3U ? "m3u"
                 : has_m3u_header                  ? "m3u/m3u8"
                                                   : "m3u-url-list";
        format_recognized = format_hint == RADIO_AUX_FORMAT_M3U ||
                            format_hint == RADIO_AUX_FORMAT_M3U8 || content_m3u;
        std::size_t position = 0U;
        std::string extinf;
        while (position < document.size() && parsed.size() < kAuxMaxStations)
        {
            const std::size_t end = document.find('\n', position);
            std::string_view line = TrimView(std::string_view(document).substr(
                position, (end == std::string::npos ? document.size() : end) - position));
            if (!line.empty() && line.size() >= 3U &&
                static_cast<unsigned char>(line[0]) == 0xefU &&
                static_cast<unsigned char>(line[1]) == 0xbbU &&
                static_cast<unsigned char>(line[2]) == 0xbfU)
                line.remove_prefix(3U);
            if (line.size() >= 8U && strncasecmp(line.data(), "#EXTINF:", 8U) == 0)
            {
                if (format_hint == RADIO_AUX_FORMAT_M3U && !IsValidUtf8(line))
                    extinf = DecodeWindows1252(line);
                else
                    extinf.assign(line.data(), line.size());
            }
            else if (!line.empty() && line.front() != '#')
            {
                const std::string url(line.data(), line.size());
                if (IsHttpUrl(url.c_str()))
                {
                    format_recognized = true;
                    AddAuxStation(parsed, url, extinf);
                }
                extinf.clear();
            }
            position = end == std::string::npos ? document.size() : end + 1U;
        }
    }

    if (!format_recognized)
    {
        std::fprintf(stderr,
                     "[ProsperoRadio][AUX] scan rejected format=unknown bytes=%llu; "
                     "previous list kept\n",
                     (unsigned long long)document.size());
        return static_cast<unsigned>(aux_stations_.size());
    }

    std::unordered_set<std::string> seen_urls;
    seen_urls.reserve(parsed.size());
    std::vector<radio_station_t> unique;
    unique.reserve(parsed.size());
    for (const radio_station_t &station : parsed)
    {
        std::string url_key(station.url);
        const std::size_t scheme_end = url_key.find("://");
        if (scheme_end != std::string::npos)
        {
            for (std::size_t i = 0; i < scheme_end; ++i)
                url_key[i] = static_cast<char>(std::tolower((unsigned char)url_key[i]));
            const std::size_t authority_begin = scheme_end + 3U;
            const std::size_t authority_end = url_key.find_first_of("/?#", authority_begin);
            const std::size_t authority_limit = authority_end == std::string::npos
                                                    ? url_key.size()
                                                    : authority_end;
            const std::size_t user_info = url_key.rfind('@', authority_limit);
            const std::size_t host_begin = user_info != std::string::npos &&
                                                   user_info >= authority_begin
                                               ? user_info + 1U
                                               : authority_begin;
            for (std::size_t i = host_begin; i < authority_limit; ++i)
                url_key[i] = static_cast<char>(std::tolower((unsigned char)url_key[i]));
        }
        if (seen_urls.emplace(std::move(url_key)).second)
            unique.push_back(station);
    }
    aux_stations_.swap(unique);
    radio_service_aux_stations_set(static_cast<int>(aux_stations_.size()));
    std::fprintf(stderr, "[ProsperoRadio][AUX] scan format=%s parsed=%u cap=%u\n", format,
                 static_cast<unsigned>(aux_stations_.size()), kAuxMaxStations);
    return static_cast<unsigned>(aux_stations_.size());
}
