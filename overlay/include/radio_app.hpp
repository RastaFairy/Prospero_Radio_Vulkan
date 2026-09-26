// ProsperoRadio - Native PlayStation 5 radio application.
// Copyright (C) 2026 BlackBearReloaded
// SPDX-License-Identifier: GPL-3.0-or-later
//
// Physical-radio frontend (01.000.019): the seven printed buttons of the
// cabinet are the navigation (D-pad moves the finger, Cross presses), the
// right dial tunes, the left dial is volume. The smoked glass shows one
// surface at a time: now playing, a station list, genres, search or settings.

#pragma once

#include "radio_input.hpp"
#include "radio_service.hpp"

#include <vector>

namespace Rml {
class ElementDocument;
}

class RadioApp {
public:
    bool Initialize(Rml::ElementDocument* document);
    void Poll();
    void HandleInput(const radio_input_event_t& event);
    void Shutdown();

private:
    enum class Mode {
        Home,     // now playing + button row navigation
        List,     // station list inside the glass (RADIO / FAVORITES buttons)
        Genres,   // genre list inside the glass
        Search,   // search overlay
        Settings, // settings panel
    };

    enum class ListKind { Radio, Favorites };

    static constexpr unsigned ButtonCount = 7;
    static constexpr unsigned ListRows = 7;
    static constexpr unsigned InvalidStation = ~0U;

    Rml::ElementDocument* document_ = nullptr;
    bool service_started_ = false;
    bool have_last_status_ = false;
    radio_service_status_t last_status_{};

    Mode mode_ = Mode::Home;
    unsigned button_focus_ = 1; // start on RADIO

    ListKind list_kind_ = ListKind::Radio;
    unsigned list_start_ = 0;
    unsigned list_cursor_ = 0;
    unsigned list_total_ = 0;
    unsigned list_indices_[ListRows]{};

    unsigned genre_start_ = 0;
    unsigned genre_cursor_ = 0;
    unsigned genre_total_ = 0;

    unsigned tuned_index_ = 0;

    bool settings_open_ = false;
    unsigned settings_focus_ = 0;
    int theme_index_ = 0;
    unsigned volume_frame_ = 20;
    unsigned tuner_state_ = 0;

    bool search_open_ = false;
    unsigned search_focus_ = 0;
    char search_query_[157]{};
    char search_edit_[157]{};
    char filter_country_[4]{};
    char filter_genre_[64]{};
    char filter_language_[64]{};
    unsigned filter_bitrate_ = 0;
    std::vector<radio_facet_t> country_facets_;
    std::vector<radio_facet_t> genre_facets_;
    std::vector<radio_facet_t> language_facets_;

    char pending_play_uuid_[40]{};

    void LoadTheme();
    void SaveTheme() const;
    void ApplyTheme();
    void CycleTheme(int direction);
    void ApplyVolumeFrame();
    void ApplyTunerFrame();
    void ApplyButtons();
    void PressButton(unsigned index);
    void UpdateFocusSearch();
    void UpdateEqualizer(const radio_service_status_t& status);

    void AdjustVolume(int direction);
    void TuneStation(int direction);
    void PlayIndex(unsigned index);
    void ToggleFavoriteOnTuned();
    void BuildList();
    void RefreshList();
    void RefreshGenres();
    void RefreshHome();
    void RefreshStatus();
    void RefreshSettings(bool refresh_favorites = true);
    void HandleSettingsKey(radio_input_key_t key);
    void OpenSearch();
    void CloseSearch(bool apply);
    void HandleSearchKey(radio_input_key_t key);
    void CycleFilter(unsigned filter, int direction);
    void UpdateSearch();
    void RebuildFacets();

    static void ImeResult(const char* text, void* user_data);
};
