// ProsperoRadio - Native PlayStation 5 radio application.
// Copyright (C) 2026 BlackBearReloaded
// SPDX-License-Identifier: GPL-3.0-or-later
//
// Physical-radio frontend (01.000.019). The cabinet artwork carries seven
// printed buttons and two dials, so the software behaves like the hardware it
// imitates: the D-pad is the finger that moves across the buttons, Cross
// presses them, the right dial tunes and the left dial is volume. The smoked
// glass shows exactly one surface: now playing, a station list, genres,
// search or settings.

#include "radio_app.hpp"
#include "radio_ime.hpp"
#include "payload_probe.hpp"

#include <RmlUi/Core/Element.h>
#include <RmlUi/Core/ElementDocument.h>

#include <algorithm>
#include <cctype>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstdlib>
#include <cstring>
#include <string>
#include <strings.h>

extern "C" uint64_t SDL_GetTicks64(void);
extern "C" uint64_t SDL_GetTicks(void);

namespace {

constexpr unsigned kButtonCount = 7;
constexpr unsigned kListRows = 7;
constexpr unsigned kInvalidStation = ~0U;
constexpr char kPresetFileMagic[8] = {'P', 'R', 'S', 'P', 'R', 'S', '0', '2'};
constexpr char kPresetFileMagicV3[8] = {'P', 'R', 'S', 'P', 'R', 'S', '0', '3'};
constexpr std::uint32_t kPresetFileVersion = 3;
constexpr char kAuxFavoriteFileMagic[8] = {'P', 'R', 'A', 'U', 'X', 'F', '0', '1'};
constexpr std::uint32_t kAuxFavoriteFileVersion = 1;
constexpr unsigned kAuxMaxStations = 4096;

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
    else
        CopyString(station->codec, sizeof(station->codec), "STREAM");

    std::uint64_t hash = UINT64_C(14695981039346656037);
    for (const unsigned char *byte = reinterpret_cast<const unsigned char *>(url); *byte; ++byte)
    {
        hash ^= *byte;
        hash *= UINT64_C(1099511628211);
    }
    std::snprintf(station->uuid, sizeof(station->uuid), "M3U-%016llX",
                  static_cast<unsigned long long>(hash));
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
    if (payload_ready)
    {
        (void)radio_payload_bridge_sync(RADIO_PAYLOAD_CATALOG);
        (void)radio_payload_bridge_sync(RADIO_PAYLOAD_FAVORITES);
        (void)radio_payload_bridge_sync(RADIO_PAYLOAD_EQ);
        (void)radio_payload_bridge_sync(RADIO_PAYLOAD_PRESETS);
        (void)radio_payload_bridge_sync(RADIO_PAYLOAD_AUXFAVORITES);
        (void)radio_payload_bridge_pull_report();
        payload_keepalive_tick_ = SDL_GetTicks64();
    }
    LoadAuxFavorites();
    radio_service_init();
    service_started_ = true;
    RebuildFacets();
    genre_total_ = static_cast<unsigned>(genre_facets_.size());
    ApplyVolumeFrame();
    ApplyTunerFrame();
    {
        std::FILE *f = std::fopen("/download0/radio-eq.txt", "rb");
        if (f)
        {
            for (int b = 0; b < 5; ++b)
            {
                int gain = 0;
                if (std::fscanf(f, "%d", &gain) != 1)
                    break;
                radio_service_eq_set_gain(b, gain);
            }
            std::fclose(f);
        }
    }
    ApplyButtons();
    BuildList();
    LoadPresets();
    ApplyPresetIndicators();
    RefreshHeadphoneState();
    RefreshHome();
    RefreshList();
    RefreshGenres();
    RefreshSettings();
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
    SetVisible(document_, "settings-panel", false);
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
        const bool selected = i != 0 && mode_ == selected_mode[i] &&
                              (selected_mode[i] != Mode::List || list_kind_ == selected_list[i]) &&
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
        if (radio_payload_bridge_fetch_aux())
            (void)ScanAuxPlaylist();
        mode_ = Mode::List;
        list_kind_ = ListKind::Radio;
        list_start_ = list_cursor_ = 0;
        BuildList();
        RefreshList();
        ApplyButtons();
        break;
    case 2: // FAVORITES
        if (radio_payload_bridge_fetch_aux())
            (void)ScanAuxPlaylist();
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
            const bool fetched = radio_payload_bridge_fetch_aux();
            const unsigned stations = fetched ? ScanAuxPlaylist() : 0U;
            char message[112];
            if (fetched && stations > 0)
                std::snprintf(message, sizeof(message), "M3U LISTA | %u EMISORAS | CROSS: ABRIR", stations);
            else if (fetched)
                CopyString(message, sizeof(message), "M3U RECIBIDA, PERO NO CONTIENE URLS HTTP VALIDAS");
            else
                CopyString(message, sizeof(message), "SIN M3U EN /DATA/RADIO - ENTRA POR AUX");
            SetText(document_, "barrido-status", message);
            std::fprintf(stderr, "[ProsperoRadio][AUX] BARRIDO fetch=%s parsed_entries=%u\n",
                         fetched ? "PASS" : "FAIL", stations);
        }
        ApplyButtons();
        break;
    case 5: // EQ — superficie visible; DSP de bandas en v027
        mode_ = Mode::Eq;
        eq_preset_ = radio_service_eq_preset();
        RefreshEq();
        ApplyButtons();
        break;
    case 6: // PLAY / PAUSE
    {
        radio_service_status_t status{};
        radio_service_get_status(&status);
        if (PlaybackActive(status.playback_state))
            radio_service_stop();
        else if ((tuned_is_aux_ && tuned_station_valid_) || status.station_count)
        {
            if (tuned_is_aux_)
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
    if (mode_ == Mode::Settings)
        RefreshSettings(false);
}

void RadioApp::TuneStation(int direction)
{
    preset_active_ = -1;
        tuned_is_aux_ = false;
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
    if (mode_ == Mode::Settings)
        RefreshSettings(false);
}

void RadioApp::PlayIndex(unsigned index)
{
    radio_station_t station{};
    if (!radio_service_get_station(index, &station))
        return;
    tuned_station_ = station;
    tuned_station_valid_ = true;
    tuned_is_aux_ = false;
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

/* --- list mode -------------------------------------------------------- */

void RadioApp::BuildList()
{
    for (unsigned &entry : list_indices_)
        entry = kInvalidStation;
    std::fill(list_aux_entries_, list_aux_entries_ + kListRows, false);
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
                        : list_kind_ == ListKind::Auxiliary ? "AUX M3U"
                                                            : "RADIO BROWSER";
    SetText(document_, "list-title", title);
    SetClass(document_, "list-tab-radio", "active", list_kind_ == ListKind::Radio);
    SetClass(document_, "list-tab-favorites", "active", list_kind_ == ListKind::Favorites);
    SetClass(document_, "list-tab-aux", "active", list_kind_ == ListKind::Auxiliary);
    char text[96];
    if (list_total_ == 0U)
    {
        SetText(document_, "list-status", list_kind_ == ListKind::Favorites
                                               ? "SIN FAVORITOS  |  □ GUARDA"
                                           : list_kind_ == ListKind::Auxiliary
                                               ? "AUX VACIA  |  ENTRA POR AUX"
                                               : "CATALOGO CARGANDO");
        for (unsigned row = 0; row < kListRows; ++row)
        {
            char id[24];
            std::snprintf(id, sizeof(id), "list-row-%u", row);
            SetVisible(document_, id, false);
        }
        return;
    }
    std::snprintf(text, sizeof(text), "%u / %u  |  ◀ ▶ FUENTE",
                  list_start_ + list_cursor_ + 1U, list_total_);
    SetText(document_, "list-status", text);
    for (unsigned row = 0; row < kListRows; ++row)
    {
        char id[24];
        std::snprintf(id, sizeof(id), "list-row-%u", row);
        if (list_start_ + row >= list_total_ || list_indices_[row] == kInvalidStation)
        {
            SetVisible(document_, id, false);
            continue;
        }
        SetVisible(document_, id, true);
        radio_station_t station{};
        bool have_station = false;
        if (list_aux_entries_[row])
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
            continue;
        }
        std::snprintf(id, sizeof(id), "list-name-%u", row);
        SetText(document_, id, station.name);
        SetClass(document_, id, "search-match",
                 *search_query_ && ContainsCi(station.name, search_query_));
        std::snprintf(id, sizeof(id), "list-meta-%u", row);
        char tag[40];
        FirstValue(station.tags, tag, sizeof(tag));
        if (list_aux_entries_[row])
            std::snprintf(text, sizeof(text), "M3U  |  %.24s  |  %s", *tag ? tag : "Radio",
                          station.codec);
        else
            std::snprintf(text, sizeof(text), "%s  |  %s %u kbps",
                          *station.country_code ? station.country_code : "WW", *tag ? tag : "Music",
                          station.bitrate);
        SetText(document_, id, text);
        std::snprintf(id, sizeof(id), "list-fav-%u", row);
        SetVisible(document_, id, list_aux_entries_[row] ? IsAuxFavorite(station.uuid)
                                                          : radio_service_is_favorite(station.uuid));
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

/* --- settings --------------------------------------------------------- */

void RadioApp::RefreshSettings(bool refresh_favorites)
{
    const unsigned volume = radio_service_get_volume();
    char text[96];
    std::snprintf(text, sizeof(text), "%u%%", volume);
    SetText(document_, "settings-volume-value", text);
    SetPixelProperty(document_, "settings-volume-fill", "width",
                     static_cast<int>((328U * volume) / 100U));
    if (refresh_favorites)
    {
        std::snprintf(text, sizeof(text), "%u saved stations",
                      radio_service_get_favorite_count());
        SetText(document_, "settings-favorites-count", text);
    }
    radio_service_status_t status{};
    radio_service_get_status(&status);
    SetText(document_, "settings-refresh-state", status.refreshing ? "Updating..." : "Ready");
    static const char *theme_names[] = {"Walnut", "Silver", "Graphite"};
    SetText(document_, "settings-theme-value",
            theme_names[theme_selected_ % (sizeof(theme_names) / sizeof(theme_names[0]))]);
    SetClass(document_, "settings-theme-row", "focused", settings_focus_ == 0U);
    SetClass(document_, "settings-favorites", "focused", settings_focus_ == 1U);
    SetClass(document_, "settings-refresh", "focused", settings_focus_ == 2U);
}

void RadioApp::HandleSettingsKey(radio_input_key_t key)
{
    if (key == RADIO_INPUT_CIRCLE || key == RADIO_INPUT_TRIANGLE)
    {
        theme_selected_ = theme_index_; // cancel a pending selection
        mode_ = Mode::Home;
        settings_open_ = false;
        ApplyButtons();
        return;
    }
    if (key == RADIO_INPUT_LEFT || key == RADIO_INPUT_RIGHT)
    {
        if (settings_focus_ == 0U)
            SelectTheme(key == RADIO_INPUT_LEFT ? -1 : 1);
        RefreshSettings();
        return;
    }
    if (key == RADIO_INPUT_UP || key == RADIO_INPUT_DOWN)
    {
        const int next = static_cast<int>(settings_focus_) + (key == RADIO_INPUT_DOWN ? 1 : -1);
        settings_focus_ = static_cast<unsigned>((next + 3) % 3);
        RefreshSettings();
        return;
    }
    if (key != RADIO_INPUT_CROSS)
        return;
    if (settings_focus_ == 0U)
    {
        if (theme_selected_ != theme_index_)
        {
            theme_index_ = theme_selected_;
            ApplyTheme();
            SaveTheme();
        }
        RefreshSettings();
        return;
    }
    if (settings_focus_ == 1U)
    {
        mode_ = Mode::List;
        list_kind_ = ListKind::Favorites;
        list_start_ = list_cursor_ = 0;
        settings_open_ = false;
        BuildList();
        RefreshList();
        ApplyButtons();
        return;
    }
    radio_service_refresh();
    RefreshSettings();
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

/* --- theme ------------------------------------------------------------ */

void RadioApp::LoadTheme()
{
    std::FILE *file = std::fopen("/download0/radio-theme.txt", "rb");
    if (file == nullptr)
        return;
    char buffer[8]{};
    const std::size_t read = std::fread(buffer, 1, sizeof(buffer) - 1, file);
    std::fclose(file);
    if (read == 0)
        return;
    if (buffer[0] >= '0' && buffer[0] <= '2')
        theme_index_ = buffer[0] - '0';
}

void RadioApp::SaveTheme() const
{
    std::FILE *file = std::fopen("/download0/radio-theme.txt", "wb");
    if (file == nullptr)
        return;
    std::fprintf(file, "%d\n", theme_index_);
    std::fclose(file);
}

void RadioApp::ApplyTheme()
{
    ApplyTheme(theme_index_);
}

void RadioApp::ApplyTheme(int index)
{
    /* Single Walnut/hybrid finish since 026: the silver/graphite backdrops are
     * gone from the RML, so the theme switch is a no-op kept for the legacy
     * call sites. The controls layer stays visible. */
    (void)index;
    SetClass(document_, "controls-layer", "hidden", false);
}

void RadioApp::SelectTheme(int direction)
{
    /* Left / right only move the picker; Cross applies and persists. */
    theme_selected_ = (theme_selected_ + 3 + (direction > 0 ? 1 : -1)) % 3;
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
    if (!event.pressed)
        return;
    /* EQ takes the left dial first: it edits the selected band's gain. */
    if (mode_ == Mode::Eq &&
        (event.key == RADIO_INPUT_VOLUME_UP || event.key == RADIO_INPUT_VOLUME_DOWN))
    {
        const int gain = radio_service_eq_gain(eq_sel_);
        radio_service_eq_set_gain(eq_sel_, gain + (event.key == RADIO_INPUT_VOLUME_UP ? 1 : -1));
        SaveEq();
        RefreshEq();
        return;
    }
    if (event.key == RADIO_INPUT_VOLUME_UP || event.key == RADIO_INPUT_VOLUME_DOWN)
    {
        AdjustVolume(event.key == RADIO_INPUT_VOLUME_UP ? 1 : -1);
        return;
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
            eq_sel_ = (eq_sel_ + 1) % 5;
            RefreshEq();
            return;
        }
        if (event.key == RADIO_INPUT_STATION_PREVIOUS || event.key == RADIO_INPUT_LEFT)
        {
            eq_sel_ = (eq_sel_ + 4) % 5;
            RefreshEq();
            return;
        }
        if (event.key == RADIO_INPUT_VOLUME_UP)
        {
            radio_service_eq_set_gain(eq_sel_, radio_service_eq_gain(eq_sel_) + 1);
            RefreshEq();
            return;
        }
        if (event.key == RADIO_INPUT_VOLUME_DOWN)
        {
            radio_service_eq_set_gain(eq_sel_, radio_service_eq_gain(eq_sel_) - 1);
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
    case Mode::Settings:
        HandleSettingsKey(event.key);
        return;
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
                else
                    list_kind_ = ListKind::Radio;
            }
            else
            {
                if (list_kind_ == ListKind::Radio)
                    list_kind_ = ListKind::Auxiliary;
                else if (list_kind_ == ListKind::Favorites)
                    list_kind_ = ListKind::Radio;
                else
                    list_kind_ = ListKind::Favorites;
            }
            list_start_ = list_cursor_ = 0;
            BuildList();
            RefreshList();
            ApplyButtons();
            return;
        }
        if (event.key == RADIO_INPUT_CROSS)
        {
            if (list_indices_[list_cursor_] != kInvalidStation)
            {
                if (list_aux_entries_[list_cursor_])
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
            if (list_aux_entries_[list_cursor_])
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
    LightbarTick();
    RefreshHeadphoneState();
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
    if (!have_last_status_ || status.catalog_state != last_status_.catalog_state ||
        status.refreshing != last_status_.refreshing ||
        status.searching != last_status_.searching ||
        status.sync_station_count != last_status_.sync_station_count ||
        status.error_code != last_status_.error_code)
    {
        if (mode_ == Mode::Settings)
            RefreshSettings(false);
    }
    UpdateEqualizer(status);
    last_status_ = status;
    have_last_status_ = true;

    const unsigned long long now = SDL_GetTicks64();
    if (preset_confirm_zone_ >= 0 && radio_input_milliseconds() >= preset_confirm_until_)
    {
        preset_confirm_zone_ = -1;
        ApplyPresetIndicators();
    }
    if (payload_keepalive_tick_ != 0 && now - payload_keepalive_tick_ >= 30000U)
    {
        (void)radio_payload_bridge_keepalive();
        payload_keepalive_tick_ = now;
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

/* --- v027: touchpad presets, lightbar pulses, EQ surface --------------- */

void RadioApp::SaveEq()
{
    std::FILE *file = std::fopen("/download0/radio-eq.txt", "wb");
    if (file)
    {
        for (int b = 0; b < 5; ++b)
            std::fprintf(file, "%d%c", radio_service_eq_gain(b), b == 4 ? '\n' : ' ');
        if (std::fclose(file) == 0)
            (void)radio_payload_bridge_push(RADIO_PAYLOAD_EQ);
    }
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
    radio_station_t station{};
    if (tuned_is_aux_)
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
    preset_external_[zone] = tuned_is_aux_;
    preset_external_station_[zone] = tuned_is_aux_ ? station : radio_station_t{};
    presets_[zone] = tuned_is_aux_ ? -1 : static_cast<int>(tuned_index_);
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
    tuned_is_aux_ = external;
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
    static const char *bands[5] = {"60Hz", "250Hz", "1kHz", "4kHz", "12kHz"};
    for (int i = 0; i < 5; ++i)
    {
        char id[16];
        std::snprintf(id, sizeof(id), "eqband-%d", i);
        char text[32];
        std::snprintf(text, sizeof(text), "%s %s%d", bands[i],
                      radio_service_eq_gain(i) > 0 ? "+" : "",
                      radio_service_eq_gain(i));
        SetText(document_, id, text);
        SetClass(document_, id, "selected", i == eq_sel_);
    }
}

void RadioApp::RefreshAuxPanel()
{
    SetText(document_, "aux-url", radio_service_aux_running()
                                      ? "AUX ACTIVO - PUERTO 7000"
                                      : "RED NO DISPONIBLE");
    const int stations = radio_service_aux_stations();
    SetText(document_, "aux-note",
            stations >= 0 ? "Entra desde el movil, envia tu M3U y pulsa BARRIDO"
                          : "Sube tu lista M3U desde el movil o el PC");
}

void RadioApp::RefreshHeadphoneState()
{
    const int state = radio_input_jack_state();
    if (state == headphone_state_)
        return;
    headphone_state_ = state;
    SetText(document_, "headphone-status",
            state < 0 ? "JACK N/A" : state ? "HEADPHONES ON" : "JACK OPEN");
    SetClass(document_, "headphone-status", "connected", state == 1);
}

unsigned RadioApp::ScanAuxPlaylist()
{
    aux_stations_.clear();
    std::FILE *file = std::fopen("/download0/radio-aux.m3u", "rb");
    if (!file)
        return 0;

    char line[4096];
    char extinf[2048]{};
    while (std::fgets(line, sizeof(line), file) != nullptr &&
           aux_stations_.size() < kAuxMaxStations)
    {
        const std::size_t length = std::strlen(line);
        if (length == sizeof(line) - 1 && line[length - 1] != '\n' && !std::feof(file))
        {
            int ch = 0;
            while ((ch = std::fgetc(file)) != '\n' && ch != EOF)
            {
            }
            extinf[0] = '\0';
            continue;
        }
        TrimLine(line);
        if (!*line)
            continue;
        if (strncasecmp(line, "#EXTINF:", 8) == 0)
        {
            CopyString(extinf, sizeof(extinf), line);
            continue;
        }
        if (line[0] == '#')
            continue;
        if (!IsHttpUrl(line))
        {
            extinf[0] = '\0';
            continue;
        }
        radio_station_t station{};
        if (std::strlen(line) >= sizeof(station.url))
        {
            extinf[0] = '\0';
            continue;
        }
        MakeAuxStation(line, extinf, &station);
        aux_stations_.push_back(station);
        extinf[0] = '\0';
    }
    std::fclose(file);
    std::fprintf(stderr, "[ProsperoRadio][AUX] M3U scan parsed=%u cap=%u\n",
                 static_cast<unsigned>(aux_stations_.size()), kAuxMaxStations);
    return static_cast<unsigned>(aux_stations_.size());
}
