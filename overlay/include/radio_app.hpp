// ProsperoRadio - Native PlayStation 5 radio application.
// Copyright (C) 2026 BlackBearReloaded
// SPDX-License-Identifier: GPL-3.0-or-later
//
// Physical-radio frontend (01.000.019): the seven printed buttons of the
// cabinet are the navigation (D-pad moves the finger, Cross presses), the
// right dial tunes, the left dial is volume. The smoked glass shows one
// surface at a time: now playing, a station list, genres, search, AUX or EQ.

#pragma once

#include "radio_input.hpp"
#include "radio_service.hpp"

#include <cstddef>
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
    bool WantsQuit() const { return quit_requested_; }

private:
    enum class Mode {
        Home,     // now playing + button row navigation
        List,     // station list inside the glass (RADIO / FAVORITES buttons)
        Genres,   // genre list inside the glass
        Search,   // search overlay
        Aux,      // external-list entry surface
        Barrido,  // list ingestion surface
        Eq,       // equalizer surface
    };

    enum class ListKind { Radio, Favorites, Auxiliary };

    static constexpr unsigned ButtonCount = 7;
    static constexpr unsigned ListRows = 7;
    static constexpr unsigned InvalidStation = ~0U;

    Rml::ElementDocument* document_ = nullptr;
    bool service_started_ = false;
    unsigned long long payload_keepalive_tick_ = 0;
    bool have_last_status_ = false;
    radio_service_status_t last_status_{};

    Mode mode_ = Mode::Home;
    unsigned button_focus_ = 1; // start on RADIO

    ListKind list_kind_ = ListKind::Radio;
    unsigned list_start_ = 0;
    unsigned list_cursor_ = 0;
    unsigned list_total_ = 0;
    unsigned list_indices_[ListRows]{};
    bool list_aux_entries_[ListRows]{};
    std::vector<radio_station_t> aux_stations_;
    std::vector<radio_station_t> aux_favorites_;
    bool aux_delete_hold_active_ = false;
    char aux_delete_hold_uuid_[40]{};
    unsigned long long aux_delete_hold_start_ = 0;
    unsigned aux_delete_display_tenth_ = ~0U;

    unsigned genre_start_ = 0;
    unsigned genre_cursor_ = 0;
    unsigned genre_total_ = 0;

    unsigned tuned_index_ = 0;

    int poweroff_ticks_ = -1;  /* >=0 while the LCD power-off fade runs */
    bool quit_requested_ = false;      /* set once, main loop breaks cooperatively (no _Exit) */
    int presets_[3] = {-1, -1, -1};   /* touchpad quick presets (station idx) */
    char preset_uuids_[3][40]{};      /* stable station identity across list pages/views */
    bool preset_saved_[3]{};
    bool preset_external_[3]{};
    radio_station_t preset_external_station_[3]{};
    radio_station_t tuned_station_{};
    bool tuned_station_valid_ = false;
    int preset_active_ = -1;          /* zone currently tuned from, -1 = free */
    bool touch_hold_active_ = false;
    bool touch_fired_ = false;
    int touch_zone_ = 0;
    unsigned long long touch_start_ = 0;
    unsigned long long lb_next_transition_ = 0;
    int lb_pulses_left_ = 0;
    bool lb_on_ = false;
    bool lb_feedback_active_ = false;
    int eq_sel_ = 0;
    int eq_preset_ = 0;
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
    unsigned pending_play_index_ = InvalidStation;
    radio_station_t pending_play_station_{};
    bool pending_play_external_ = false;
    bool tuned_is_aux_ = false;
    int preset_confirm_zone_ = -1;
    unsigned long long preset_confirm_until_ = 0;

    void ApplyVolumeFrame();
    void ApplyTunerFrame();
    void ApplyButtons();
    void ShowScreen();
    void PressButton(unsigned index);
    void RefreshVolumeDisplay();
    void UpdateFocusSearch();
    void UpdateEqualizer(const radio_service_status_t& status);

    void StorePreset(int zone);
    void RecallPreset(int zone);
    void StartPresetFeedback(int zone);
    void ApplyPresetIndicators();
    void LoadPresets();
    bool SavePresets();
    void LoadAuxFavorites();
    bool SaveAuxFavorites();
    bool IsAuxFavorite(const char *uuid) const;
    bool ToggleAuxFavorite(const radio_station_t &station);
    bool SaveAuxPlaylist(const std::vector<radio_station_t> &stations,
                         const char *excluded_uuid = nullptr);
    bool FetchAuxPlaylist();
    bool DeleteAuxStation(const char *uuid);
    void BeginAuxDeleteHold();
    void CancelAuxDeleteHold();
    void ReleaseAuxDeleteHold();
    void UpdateAuxDeleteHold();
    void LightbarTick();
    void LoadEq();
    void RefreshEq();
    void SaveEq();
    void RefreshAuxPanel();
    void AdjustVolume(int direction);
    void TuneStation(int direction);
    void PlayIndex(unsigned index);
    void PlayAuxStation(const radio_station_t &station);
    unsigned ScanAuxPlaylist(const unsigned char *data, std::size_t size,
                             unsigned char format);
    void ToggleFavoriteOnTuned();
    void BuildList();
    void RefreshList();
    void RefreshGenres();
    void RefreshHome();
    void RefreshStatus();
    void OpenSearch();
    void CloseSearch(bool apply);
    void HandleSearchKey(radio_input_key_t key);
    void CycleFilter(unsigned filter, int direction);
    void UpdateSearch();
    void RebuildFacets();

    static void ImeResult(const char* text, void* user_data);
};
