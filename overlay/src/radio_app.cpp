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

#include <RmlUi/Core/Element.h>
#include <RmlUi/Core/ElementDocument.h>

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>

extern "C" uint64_t SDL_GetTicks64(void);
extern "C" uint64_t SDL_GetTicks(void);

namespace {

constexpr unsigned kButtonCount = 7;
constexpr unsigned kListRows = 7;
constexpr unsigned kInvalidStation = ~0U;

void SetText(Rml::ElementDocument *document, const char *id, const char *value)
{
    if (Rml::Element *element = document->GetElementById(id))
        element->SetInnerRML(value ? value : "");
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
    radio_service_init();
    service_started_ = true;
    RebuildFacets();
    genre_total_ = static_cast<unsigned>(genre_facets_.size());
    LoadTheme();
    ApplyTheme();
    ApplyVolumeFrame();
    ApplyTunerFrame();
    ApplyButtons();
    BuildList();
    RefreshHome();
    RefreshList();
    RefreshGenres();
    RefreshSettings();
    return true;
}

void RadioApp::Shutdown()
{
    if (service_started_)
    {
        radio_service_shutdown();
        service_started_ = false;
    }
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

    if (!genre_facets_.empty())
        return;

    auto add = [](std::vector<radio_facet_t> &facets, const char *value)
    {
        if (!*value)
            return;
        for (radio_facet_t &facet : facets)
        {
            if (std::strcmp(facet.value, value) == 0)
            {
                ++facet.station_count;
                return;
            }
        }
        if (facets.size() >= RADIO_MAX_FACETS)
            return;
        radio_facet_t facet{};
        CopyString(facet.value, sizeof(facet.value), value);
        CopyString(facet.label, sizeof(facet.label), value);
        facet.station_count = 1;
        facets.push_back(facet);
    };
    radio_service_status_t status{};
    radio_service_get_status(&status);
    for (unsigned i = 0; i < status.station_count; ++i)
    {
        radio_station_t station{};
        if (!radio_service_get_station(i, &station))
            continue;
        char value[64];
        FirstValue(station.tags, value, sizeof(value));
        add(genre_facets_, value);
    }
    std::sort(genre_facets_.begin(), genre_facets_.end(),
              [](const radio_facet_t &left, const radio_facet_t &right)
              { return left.station_count > right.station_count; });
}

/* --- buttons ---------------------------------------------------------- */

void RadioApp::ApplyButtons()
{
    static const char *names[] = {"home", "radio", "favorites", "genres",
                                  "search", "settings", "play-pause"};
    static const char *states[] = {"normal", "focus", "pressed", "selected", "selected_focus"};
    static const Mode selected_mode[] = {
        Mode::Home,   Mode::List,    Mode::List,     Mode::Genres,
        Mode::Search, Mode::Settings, Mode::Home,
    };
    static const ListKind selected_list[] = {
        ListKind::Radio, ListKind::Radio, ListKind::Favorites, ListKind::Radio,
        ListKind::Radio, ListKind::Radio,  ListKind::Radio,
    };
    radio_service_status_t status{};
    radio_service_get_status(&status);
    for (unsigned i = 0; i < kButtonCount; ++i)
    {
        const bool selected = mode_ == selected_mode[i] &&
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
    switch (index)
    {
    case 0: // HOME
        mode_ = Mode::Home;
        ApplyButtons();
        RefreshHome();
        break;
    case 1: // RADIO
        mode_ = Mode::List;
        list_kind_ = ListKind::Radio;
        list_start_ = list_cursor_ = 0;
        BuildList();
        RefreshList();
        ApplyButtons();
        break;
    case 2: // FAVORITES
        mode_ = Mode::List;
        list_kind_ = ListKind::Favorites;
        list_start_ = list_cursor_ = 0;
        BuildList();
        RefreshList();
        ApplyButtons();
        break;
    case 3: // GENRES
        mode_ = Mode::Genres;
        genre_start_ = genre_cursor_ = 0;
        RefreshGenres();
        ApplyButtons();
        break;
    case 4: // SEARCH
        OpenSearch();
        break;
    case 5: // SETTINGS
        mode_ = Mode::Settings;
        settings_open_ = true;
        settings_focus_ = 0;
        RefreshSettings();
        ApplyButtons();
        break;
    case 6: // PLAY / PAUSE
    {
        radio_service_status_t status{};
        radio_service_get_status(&status);
        if (PlaybackActive(status.playback_state))
            radio_service_stop();
        else if (status.station_count)
            PlayIndex(tuned_index_);
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
    }
    if (mode_ == Mode::Settings)
        RefreshSettings(false);
}

void RadioApp::TuneStation(int direction)
{
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
    radio_service_status_t status{};
    radio_service_get_status(&status);
    if (PlaybackActive(status.playback_state))
    {
        if (radio_service_station_is_playing(index))
            return;
        pending_play_uuid_[0] = '\0';
        CopyString(pending_play_uuid_, sizeof(pending_play_uuid_), station.uuid);
        radio_service_stop();
        return;
    }
    radio_service_play(index);
}

/* --- list mode -------------------------------------------------------- */

void RadioApp::BuildList()
{
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
    for (unsigned &entry : list_indices_)
        entry = kInvalidStation;
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
    static const char *titles[] = {"RADIO BROWSER", "FAVORITES"};
    SetText(document_, "list-title",
            titles[list_kind_ == ListKind::Favorites ? 1 : 0]);
    char text[96];
    if (list_total_ == 0U)
    {
        SetText(document_, "list-status",
                list_kind_ == ListKind::Favorites ? "No saved stations yet" : "Catalog loading");
        for (unsigned row = 0; row < kListRows; ++row)
        {
            char id[24];
            std::snprintf(id, sizeof(id), "list-row-%u", row);
            SetVisible(document_, id, false);
        }
        return;
    }
    std::snprintf(text, sizeof(text), "%u / %u STATIONS", list_start_ + list_cursor_ + 1U,
                  list_total_);
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
        if (!radio_service_get_station(list_indices_[row], &station))
        {
            SetVisible(document_, id, false);
            continue;
        }
        std::snprintf(id, sizeof(id), "list-name-%u", row);
        SetText(document_, id, station.name);
        std::snprintf(id, sizeof(id), "list-meta-%u", row);
        char tag[40];
        FirstValue(station.tags, tag, sizeof(tag));
        std::snprintf(text, sizeof(text), "%s  |  %s %u kbps",
                      *station.country_code ? station.country_code : "WW", *tag ? tag : "Music",
                      station.bitrate);
        SetText(document_, id, text);
        std::snprintf(id, sizeof(id), "list-fav-%u", row);
        SetVisible(document_, id, radio_service_is_favorite(station.uuid));
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
    const bool have = radio_service_get_station(tuned_index_, &station);
    if (have)
    {
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
            theme_names[theme_index_ % (sizeof(theme_names) / sizeof(theme_names[0]))]);
    SetClass(document_, "settings-theme-row", "focused", settings_focus_ == 0U);
    SetClass(document_, "settings-favorites", "focused", settings_focus_ == 1U);
    SetClass(document_, "settings-refresh", "focused", settings_focus_ == 2U);
}

void RadioApp::HandleSettingsKey(radio_input_key_t key)
{
    if (key == RADIO_INPUT_CIRCLE || key == RADIO_INPUT_TRIANGLE)
    {
        mode_ = Mode::Home;
        settings_open_ = false;
        SetVisible(document_, "settings-panel", false);
        SetVisible(document_, "screen-home", true);
        ApplyButtons();
        return;
    }
    if (key == RADIO_INPUT_LEFT || key == RADIO_INPUT_RIGHT)
    {
        if (settings_focus_ == 0U)
            CycleTheme(key == RADIO_INPUT_LEFT ? -1 : 1);
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
        CycleTheme(1);
        return;
    }
    if (settings_focus_ == 1U)
    {
        mode_ = Mode::List;
        list_kind_ = ListKind::Favorites;
        list_start_ = list_cursor_ = 0;
        settings_open_ = false;
        SetVisible(document_, "settings-panel", false);
        SetVisible(document_, "screen-list", true);
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
    SetVisible(document_, "search-panel", true);
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
    SetVisible(document_, "search-panel", false);
    SetVisible(document_, "screen-home", true);
    if (apply)
    {
        CopyString(search_query_, sizeof(search_query_), search_edit_);
        mode_ = Mode::List;
        list_kind_ = ListKind::Radio;
        list_start_ = list_cursor_ = 0;
        SetVisible(document_, "screen-list", true);
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
    SetClass(document_, "filter-2", "focused", search_focus_ == 2);
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
    SetClass(document_, "radio-backdrop", "backdrop-hidden", theme_index_ != 0);
    SetClass(document_, "radio-backdrop-silver", "backdrop-hidden", theme_index_ != 1);
    SetClass(document_, "radio-backdrop-graphite", "backdrop-hidden", theme_index_ != 2);
    SetClass(document_, "controls-layer", "hidden", theme_index_ != 0);
}

void RadioApp::CycleTheme(int direction)
{
    theme_index_ = (theme_index_ + 3 + (direction > 0 ? 1 : -1)) % 3;
    ApplyTheme();
    SaveTheme();
}

/* --- atlas frames ----------------------------------------------------- */

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
    if (!event.pressed)
        return;
    if (event.key == RADIO_INPUT_VOLUME_UP || event.key == RADIO_INPUT_VOLUME_DOWN)
    {
        AdjustVolume(event.key == RADIO_INPUT_VOLUME_UP ? 1 : -1);
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
                SetVisible(document_, "screen-genres", false);
                SetVisible(document_, "screen-list", true);
                BuildList();
                RefreshList();
                ApplyButtons();
            }
            return;
        }
        if (event.key == RADIO_INPUT_CIRCLE || event.key == RADIO_INPUT_TRIANGLE)
        {
            mode_ = Mode::Home;
            SetVisible(document_, "screen-genres", false);
            SetVisible(document_, "screen-home", true);
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
            list_kind_ = list_kind_ == ListKind::Radio ? ListKind::Favorites : ListKind::Radio;
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
                tuned_index_ = list_indices_[list_cursor_];
                PlayIndex(tuned_index_);
                RefreshHome();
            }
            return;
        }
        if (event.key == RADIO_INPUT_SQUARE)
        {
            if (list_indices_[list_cursor_] != kInvalidStation)
            {
                radio_service_toggle_favorite(list_indices_[list_cursor_]);
                RefreshList();
            }
            return;
        }
        if (event.key == RADIO_INPUT_CIRCLE || event.key == RADIO_INPUT_TRIANGLE)
        {
            mode_ = Mode::Home;
            SetVisible(document_, "screen-list", false);
            SetVisible(document_, "screen-home", true);
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
    radio_service_status_t status{};
    radio_service_get_status(&status);
    if (!have_last_status_ || status.catalog_generation != last_status_.catalog_generation ||
        status.station_count != last_status_.station_count)
    {
        RebuildFacets();
        genre_total_ = static_cast<unsigned>(genre_facets_.size());
        RefreshGenres();
        if (mode_ == Mode::List)
        {
            BuildList();
            RefreshList();
        }
        RefreshHome();
    }
    if (*pending_play_uuid_ && status.playback_state == RADIO_PLAYBACK_STOPPED)
    {
        char pending_uuid[sizeof(pending_play_uuid_)]{};
        CopyString(pending_uuid, sizeof(pending_uuid), pending_play_uuid_);
        pending_play_uuid_[0] = '\0';
        for (unsigned i = 0; i < status.station_count; ++i)
        {
            radio_station_t station{};
            if (radio_service_get_station(i, &station) &&
                std::strcmp(station.uuid, pending_uuid) == 0)
            {
                radio_service_play(i);
                radio_service_get_status(&status);
                break;
            }
        }
    }
    if (!have_last_status_ || status.playback_state != last_status_.playback_state ||
        status.playing_index != last_status_.playing_index ||
        status.sample_rate != last_status_.sample_rate ||
        status.channels != last_status_.channels || status.error_code != last_status_.error_code)
        RefreshStatus();
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
