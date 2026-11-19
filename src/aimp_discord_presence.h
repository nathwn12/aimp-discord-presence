//  Copyright (c) 2024 Exle
//
//  Permission is hereby granted, free of charge, to any person obtaining a
//  copy of this software and associated documentation files (the "Software"),
//  to deal in the Software without restriction, including without limitation
//  the rights to use, copy, modify, merge, publish, distribute, sublicense,
//  and/or sell copies of the Software, and to permit persons to whom the
//  Software is furnished to do so, subject to the following conditions:
//
//  The above copyright notice and this permission notice shall be included in
//  all copies or substantial portions of the Software.
//
//  THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS
//  OR IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
//  FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL THE
//  AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
//  LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING
//  FROM, OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER
//  DEALINGS IN THE SOFTWARE.

#ifndef AIMPDISCORDPRESENCE_SRC_AIMP_DISCORD_PRESENCE_H_
#define AIMPDISCORDPRESENCE_SRC_AIMP_DISCORD_PRESENCE_H_

#include <atomic>
#include <condition_variable>
#include <cstdint>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <unordered_map>
#include <vector>

#include "aimp_implements.h"
#include "aimp_plugin.h"
#include "aimp_core.h"

#include "album_art.h"
#include "discord_ipc.h"
#include "local_art.h"
#include "presence_layout.h"
#include "presence_update_gate.h"

class AimpDiscordPresence :
  public Aimp::Implements<Aimp::Plugin, Aimp::ExternalSettingsDialog> {
 public:
  PWCHAR GetInfo(int index) override;
  DWORD GetCategory() override;
  bool Load() override;
  bool Unload() override;
  void Notification(int id, IUnknown* data) override;
  void ShowSettings(HWND parent_wnd) override;

  // Routes a diagnostic line to the DebugLog client. Public so the publisher's
  // plain-function-pointer log hook can forward here without an instance.
  void LogCover(const std::string& line);

 private:
  struct TrackInfo {
    std::string key;
    std::string title;
    std::string artist;
    std::string album;
    std::string year;
    std::string genre;
    std::string file;
    bool is_url = false;
  };

  void OnStreamStartSubtrack();
  void OnPlayerState(DWORD message, int param1 = NULL);
  void OnPropertyValue(DWORD message, int param1 = NULL);
  void OnPlayerUpdatePosition();

 private:
  void InitializeMessageDispatcher();

 private:
  void RefreshPresence(bool request_artwork);
  void SendActivity();

  void StartThrottleWorker();
  void StopThrottleWorker();
  void ThrottleWorkerMain();
  void QueueClearLocked();
  void ClearPresenceBeforeStop();

  TrackInfo ReadTrackInfo();

  void SetInfo(const TrackInfo& info);
  void SetSmallImage(const TrackInfo& info, int state = -1);
  void SetTimestamp(const TrackInfo& info);

  void ApplyResolvedArtwork(const std::string& artist, const std::string& album,
                            const std::string& file_path, const std::string& url);
  bool ApplyPendingArtwork();
  bool HasPendingCoverSource();

  LocalArt::Result ExtractLocalArt(const TrackInfo& info, bool want_bytes);
  void MaybePublishLocalCover(const TrackInfo& info, LocalArt::Result art);
  void CoverWorkerMain();
  void StopCoverWorker();

  std::string ResolveLargeImageLocked(const std::string& album_key,
                                      std::string* source) const;

 private:
  void LoadConfig();

  template <typename T>
  void LoadConfigValue(Aimp::Core::Service::Config config, const std::wstring& key, T value);

  struct Properties {
    int64_t application_id = 429559336982020107LL;
    bool timestamp = true;
    bool use_albumart = true;
    bool use_albumart_online = true;
    bool local_cover = true;
    std::wstring cover_cache;
    std::wstring cover_host = L"auto";
    std::wstring drive_client_id;
    std::wstring drive_client_secret;
    int status_display_type = 2;
    std::wstring name = L"Music";
    std::wstring details_template = L"{title}";
    std::wstring state_template = L"{artist}";
    std::wstring large_text_template = L"{album}";
    std::wstring button1_label = L"Listen on Spotify";
    std::wstring button1_url = L"https://open.spotify.com/search/";
    std::wstring button2_label = L"Find on YouTube";
    std::wstring button2_url = L"https://music.youtube.com/search?q=";
    int throttle_ms = 4000;
    std::wstring debug_log;
    struct State {
      bool use_play = false;
      std::wstring play_image = L"aimp_play";
      std::wstring play_text = L"Playing";
      bool use_pause = false;
      std::wstring pause_image = L"aimp_pause";
      std::wstring pause_text = L"Paused";
      bool use_radio = true;
      std::wstring radio_image = L"https://raw.githubusercontent.com/Exle/aimp-discord-presence/main/"
                           L".github/aimp_icons/animated/aimp_radio.gif";
      std::wstring radio_text;
    };
    State status;
  };

  Properties settings;

  std::unique_ptr<DiscordIpc::Client> client_;
  AlbumArt::Resolver album_art_;

  std::mutex presence_mutex_;
  DiscordIpc::Activity activity_;
  std::string artwork_key_;
  std::string artwork_url_;
  std::string local_cover_key_;
  std::string local_cover_url_;

  std::string track_key_;
  std::string track_artist_;
  std::string track_album_;
  std::string track_file_;
  PresenceUpdateGate update_gate_;
  bool online_requested_ = false;
  PresenceLayout::TrackFields track_fields_;
  std::string last_large_image_;
  std::string local_art_key_;
  std::string local_cover_sha_;
  std::unordered_map<std::string, std::string> local_cover_urls_;
  std::atomic<double> sent_position_{0.0};
  std::atomic<int64_t> sent_at_seconds_{0};
  bool paused_ = false;

  std::thread cover_worker_;
  std::mutex cover_mutex_;
  std::condition_variable cover_wake_;
  bool cover_stopping_ = false;
  bool cover_has_request_ = false;
  std::string cover_request_hash_;
  std::string cover_request_mime_;
  std::vector<unsigned char> cover_request_bytes_;
  std::string cover_inflight_hash_;
  bool cover_result_ready_ = false;
  bool cover_result_ok_ = false;
  std::string cover_result_hash_;
  std::string cover_result_url_;
  std::string cover_result_reason_;
  std::string cover_result_detail_;
  std::string cover_result_host_;

  std::thread throttle_worker_;
  std::mutex throttle_mutex_;
  std::condition_variable throttle_wake_;
  bool throttle_stopping_ = false;
  bool desired_valid_ = false;
  uint64_t desired_revision_ = 0;
  uint64_t last_sent_revision_ = 0;
  uint64_t throttle_last_sent_ms_ = 0;
  DiscordIpc::Activity desired_activity_;
  double desired_position_ = 0.0;
  int64_t desired_at_seconds_ = 0;
};

#endif  // AIMPDISCORDPRESENCE_SRC_AIMP_DISCORD_PRESENCE_H_
