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

#include "aimp_discord_presence.h"

#include <chrono>
#include <cmath>
#include <string>
#include <utility>
#include <vector>

#include "aimp_core.h"
#include "aimp_filemanager.h"
#include "aimp_messages.h"
#include "aimp_player.h"
#include "cover_publisher.h"
#include "presence_layout.h"
#include "presence_rate_limit.h"
#include "presence_throttle_plan.h"
#include "utils.h"

namespace {

AimpDiscordPresence* g_publisher_log_target = nullptr;

void PublisherLogTrampoline(const std::string& line) {
  if (g_publisher_log_target != nullptr) {
    g_publisher_log_target->LogCover(line);
  }
}

constexpr int kPlayerStateStopped = 0;
constexpr int kPlayerStatePaused = 1;
constexpr int kPlayerStatePlaying = 2;

constexpr double kSeekDriftSeconds = 3.0;

constexpr size_t kMaxCoverBytes = 20 * 1024 * 1024;

constexpr int kPresenceClearOnUnloadTimeoutMs = 2000;

int64_t UnixSecondsNow() {
  return std::chrono::duration_cast<std::chrono::seconds>(
             std::chrono::system_clock::now().time_since_epoch())
      .count();
}

uint64_t MonotonicMillisNow() {
  return static_cast<uint64_t>(GetTickCount64());
}

bool IsStreamUrl(const std::wstring& url) {
  if (url.size() >= 7 && url.compare(0, 7, L"http://") == 0) {
    return true;
  }
  return url.size() >= 8 && url.compare(0, 8, L"https://") == 0;
}

bool IsAbsolutePath(const std::wstring& path) {
  if (path.size() >= 3 && path[1] == L':' &&
      (path[2] == L'\\' || path[2] == L'/')) {
    return true;
  }
  return path.size() >= 2 && (path[0] == L'\\' || path[0] == L'/') &&
         (path[1] == L'\\' || path[1] == L'/');
}

std::string PluginDirectory() {
  HMODULE module = nullptr;
  if (!GetModuleHandleExA(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS |
                              GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
                          reinterpret_cast<LPCSTR>(&PluginDirectory),
                          &module)) {
    return std::string();
  }

  char module_path[MAX_PATH] = {};
  const DWORD length = GetModuleFileNameA(module, module_path, MAX_PATH);
  if (length == 0 || length >= MAX_PATH) {
    return std::string();
  }

  std::string directory(module_path, length);
  const size_t separator = directory.find_last_of("\\/");
  if (separator == std::string::npos) {
    return std::string();
  }

  return directory.substr(0, separator + 1);
}

std::string ResolveDebugLogPath(const std::wstring& configured) {
  if (configured.empty()) {
    return std::string();
  }
  if (IsAbsolutePath(configured)) {
    return Utils::ToString(configured);
  }

  const std::string directory = PluginDirectory();
  if (directory.empty()) {
    return std::string();
  }

  return directory + Utils::ToString(configured);
}

std::string ResolveCoverCachePath(const std::wstring& configured) {
  if (IsAbsolutePath(configured)) {
    return Utils::ToString(configured);
  }

  const std::string directory = PluginDirectory();
  if (directory.empty()) {
    return std::string();
  }

  return directory + Utils::ToString(configured.empty() ? L"cover-cache.txt"
                                                        : configured);
}

std::string ResolveCoverStatePath(const std::string& cover_cache_path) {
  if (cover_cache_path.empty()) {
    return std::string();
  }
  const size_t separator = cover_cache_path.find_last_of("\\/");
  if (separator == std::string::npos) {
    return std::string();
  }
  return cover_cache_path.substr(0, separator + 1) + "drive-token.txt";
}

}  // namespace

__declspec(dllexport) HRESULT WINAPI AIMPPluginGetHeader(void** Header) {
  *Header = new AimpDiscordPresence();
  return S_OK;
}

LPWSTR AimpDiscordPresence::GetInfo(int index) {
  switch (index) {
    case Info::kName:
      return const_cast<LPWSTR>(L"Discord Presence");
    case Info::kAuthor:
      return const_cast<LPWSTR>(L"Exle (original), nathwn12 (fork)");
    case Info::Description::kShort:
      return const_cast<LPWSTR>(L"Discord Rich Presence for AIMP \u2014 fork of Exle's plugin");
  }

  return nullptr;
}

DWORD AimpDiscordPresence::GetCategory() {
  return Category::kAddons;
}

bool AimpDiscordPresence::Load() {
  if (!Aimp::Player::Service::Player()) {
    return false;
  }

  LoadConfig();
  InitializeMessageDispatcher();

  client_.reset(new DiscordIpc::Client(std::to_string(settings.application_id)));

  if (!settings.debug_log.empty()) {
    client_->SetLogPath(ResolveDebugLogPath(settings.debug_log));
  }

  const std::string cover_cache_path = ResolveCoverCachePath(settings.cover_cache);
  CoverPublisher::Configure(cover_cache_path);
  CoverPublisher::ConfigureHost(Utils::ToString(settings.cover_host));
  CoverPublisher::ConfigureDrive(Utils::ToString(settings.drive_client_id),
                                 Utils::ToString(settings.drive_client_secret),
                                 ResolveCoverStatePath(cover_cache_path));
  g_publisher_log_target = this;
  CoverPublisher::SetLogger(&PublisherLogTrampoline);
  CoverPublisher::LogHostConfiguration();

  cover_worker_ = std::thread(&AimpDiscordPresence::CoverWorkerMain, this);

  StartThrottleWorker();

  album_art_.SetCallback(
      [this](const std::string& artist, const std::string& album,
             const std::string& file_path, const std::string& url) {
        ApplyResolvedArtwork(artist, album, file_path, url);
      });
  album_art_.SetLogger([this](const std::string& line) { LogCover(line); });
  album_art_.SetOnlineEnabled(settings.use_albumart_online);

  client_->EnsureConnected();
  RefreshPresence(true);

  return true;
}

void AimpDiscordPresence::LoadConfig() {
  Aimp::Core::Service::Config config;

  LoadConfigValue(config, L"DiscordPresence\\ApplicationID", &settings.application_id);
  LoadConfigValue(config, L"DiscordPresence\\Timestamp", &settings.timestamp);
  LoadConfigValue(config, L"DiscordPresence\\UseAlbumArt", &settings.use_albumart);
  LoadConfigValue(config, L"DiscordPresence\\UseAlbumArtOnline", &settings.use_albumart_online);
  LoadConfigValue(config, L"DiscordPresence\\LocalCover", &settings.local_cover);
  LoadConfigValue(config, L"DiscordPresence\\CoverCache", &settings.cover_cache);
  LoadConfigValue(config, L"DiscordPresence\\CoverHost", &settings.cover_host);
  LoadConfigValue(config, L"DiscordPresence\\DriveClientId", &settings.drive_client_id);
  LoadConfigValue(config, L"DiscordPresence\\DriveClientSecret", &settings.drive_client_secret);
  LoadConfigValue(config, L"DiscordPresence\\StatusDisplayType", &settings.status_display_type);
  LoadConfigValue(config, L"DiscordPresence\\Name", &settings.name);
  LoadConfigValue(config, L"DiscordPresence\\Details", &settings.details_template);
  LoadConfigValue(config, L"DiscordPresence\\State", &settings.state_template);
  LoadConfigValue(config, L"DiscordPresence\\LargeText", &settings.large_text_template);
  LoadConfigValue(config, L"DiscordPresence\\Button1.Label", &settings.button1_label);
  LoadConfigValue(config, L"DiscordPresence\\Button1.Url", &settings.button1_url);
  LoadConfigValue(config, L"DiscordPresence\\Button2.Label", &settings.button2_label);
  LoadConfigValue(config, L"DiscordPresence\\Button2.Url", &settings.button2_url);
  LoadConfigValue(config, L"DiscordPresence\\ThrottleMs", &settings.throttle_ms);
  LoadConfigValue(config, L"DiscordPresence\\DebugLog", &settings.debug_log);
  LoadConfigValue(config, L"DiscordPresence\\State.UsePlay", &settings.status.use_play);
  LoadConfigValue(config, L"DiscordPresence\\State.PlayImage", &settings.status.play_image);
  LoadConfigValue(config, L"DiscordPresence\\State.PlayText", &settings.status.play_text);
  LoadConfigValue(config, L"DiscordPresence\\State.UsePause", &settings.status.use_pause);
  LoadConfigValue(config, L"DiscordPresence\\State.PauseImage", &settings.status.pause_image);
  LoadConfigValue(config, L"DiscordPresence\\State.PauseText", &settings.status.pause_text);
  LoadConfigValue(config, L"DiscordPresence\\State.UseRadio", &settings.status.use_radio);
  LoadConfigValue(config, L"DiscordPresence\\State.RadioImage", &settings.status.radio_image);
  LoadConfigValue(config, L"DiscordPresence\\State.RadioText", &settings.status.radio_text);
}

void AimpDiscordPresence::InitializeMessageDispatcher() {
  Aimp::Messages::Service::MessageDispatcher message_dispatcher;

  message_dispatcher.Hook(Aimp::Messages::Events::kPlayerState,
                          [&](DWORD message, int param1, void*, HRESULT*) {
                            OnPlayerState(message, param1);
                          });

  message_dispatcher.Hook(Aimp::Messages::Events::Stream::Start::kSubtrack,
                          [&](DWORD, int, void*, HRESULT*) {
                            OnStreamStartSubtrack();
                          });

  message_dispatcher.Hook(Aimp::Messages::Events::kPropertyValue,
                          [&](DWORD message, int param1, void*, HRESULT*) {
                            OnPropertyValue(message, param1);
                          });

  message_dispatcher.Hook(Aimp::Messages::Events::kPlayerUpdatePosition,
                          [&](DWORD, int, void*, HRESULT*) {
                            OnPlayerUpdatePosition();
                          });
}

template<typename T>
void AimpDiscordPresence::LoadConfigValue(Aimp::Core::Service::Config config, const std::wstring& key, T value) {
  if (!config.Get(key, value)) {
    config.Set(key, *value);
  }
}

bool AimpDiscordPresence::Unload() {
  Aimp::Messages::Service::MessageDispatcher().UnhookAll();

  CoverPublisher::SetLogger(nullptr);
  g_publisher_log_target = nullptr;

  CoverPublisher::CancelDriveConsent();
  StopCoverWorker();

  album_art_.Shutdown();

  ClearPresenceBeforeStop();
  StopThrottleWorker();

  if (client_) {
    client_->Shutdown();
    client_.reset();
  }

  return true;
}

void AimpDiscordPresence::Notification(int, IUnknown*) {}
void AimpDiscordPresence::ShowSettings(HWND) {}

void AimpDiscordPresence::OnPlayerState(DWORD, int param1) {
  if (param1 == kPlayerStateStopped ||
      (param1 == kPlayerStatePaused && !settings.status.use_pause)) {
    track_key_.clear();
    online_requested_ = false;
    update_gate_.Reset();
    {
      std::lock_guard<std::mutex> lock(throttle_mutex_);
      sent_at_seconds_.store(0, std::memory_order_relaxed);
      QueueClearLocked();
    }
    throttle_wake_.notify_one();
    return;
  }

  paused_ = (param1 == kPlayerStatePaused);
  RefreshPresence(true);
}

void AimpDiscordPresence::OnStreamStartSubtrack() {
  Aimp::Player::Service::Player player;
  paused_ = (player.State() == kPlayerStatePaused);
  RefreshPresence(true);
}

void AimpDiscordPresence::OnPropertyValue(DWORD, int param1) {
  if (param1 != Aimp::Messages::Properties::Player::kPosition) {
    return;
  }

  Aimp::Player::Service::Player player;
  if (player.State() != kPlayerStatePlaying) {
    return;
  }

  const TrackInfo info = ReadTrackInfo();
  if (info.key != track_key_) {
    RefreshPresence(true);
    return;
  }

  if (sent_at_seconds_.load(std::memory_order_relaxed) != 0) {
    const double elapsed = static_cast<double>(
        UnixSecondsNow() - sent_at_seconds_.load(std::memory_order_relaxed));
    if (std::fabs(player.Position() -
                  (sent_position_.load(std::memory_order_relaxed) + elapsed)) >
        kSeekDriftSeconds) {
      RefreshPresence(false);
      return;
    }
  }

  const bool artwork_applied = ApplyPendingArtwork();
  const uint64_t now_ms = MonotonicMillisNow();
  if (update_gate_.HasPending()) {
    PresenceUpdateGate::Decision release;
    if (artwork_applied) {
      release = update_gate_.OnArtworkReady(now_ms);
    } else if (!HasPendingCoverSource()) {
      release = update_gate_.OnDefinitiveMiss(now_ms);
    } else {
      release = update_gate_.OnTick(now_ms);
    }
    if (release == PresenceUpdateGate::Decision::Send) {
      SendActivity();
    }
    return;
  }
  if (artwork_applied) {
    update_gate_.NoteLateCover(now_ms);
    SendActivity();
  }
}

void AimpDiscordPresence::OnPlayerUpdatePosition() {
  Aimp::Player::Service::Player player;
  if (player.State() != kPlayerStatePlaying) {
    return;
  }

  const bool artwork_applied = ApplyPendingArtwork();
  const uint64_t now_ms = MonotonicMillisNow();
  if (update_gate_.HasPending()) {
    PresenceUpdateGate::Decision release;
    if (artwork_applied) {
      release = update_gate_.OnArtworkReady(now_ms);
    } else if (!HasPendingCoverSource()) {
      release = update_gate_.OnDefinitiveMiss(now_ms);
    } else {
      release = update_gate_.OnTick(now_ms);
    }
    if (release == PresenceUpdateGate::Decision::Send) {
      SendActivity();
    }
    return;
  }
  if (artwork_applied) {
    update_gate_.NoteLateCover(now_ms);
    SendActivity();
  }
}

AimpDiscordPresence::TrackInfo AimpDiscordPresence::ReadTrackInfo() {
  TrackInfo info;

  Aimp::Player::Service::Player player;
  Aimp::FileManager::FileInfo fileinfo = player.GetInfo();

  const std::wstring file_name =
      fileinfo.Get<std::wstring>(Aimp::FileManager::FileInfo::Props::kFileName);
  info.title = Utils::ToString(fileinfo.Get<std::wstring>(Aimp::FileManager::FileInfo::Props::kTitle));
  info.artist = PresenceLayout::NormalizeArtistSeparators(
      Utils::ToString(fileinfo.Get<std::wstring>(Aimp::FileManager::FileInfo::Props::kArtist)));
  info.album = Utils::ToString(fileinfo.Get<std::wstring>(Aimp::FileManager::FileInfo::Props::kAlbum));
  info.year = Utils::ToString(
      fileinfo.Get<std::wstring>(Aimp::FileManager::FileInfo::Props::kDate));
  info.genre = Utils::ToString(
      fileinfo.Get<std::wstring>(Aimp::FileManager::FileInfo::Props::kGenre));
  info.file = Utils::ToString(file_name);
  info.is_url = IsStreamUrl(file_name);
  info.key = info.artist + "\n" + info.album + "\n" + info.title;

  return info;
}

void AimpDiscordPresence::RefreshPresence(bool request_artwork) {
  const TrackInfo info = ReadTrackInfo();

  if (info.file.empty()) {
    online_requested_ = false;
    update_gate_.Reset();
    return;
  }

  bool local_art_found = false;
  bool local_published = false;
  if (request_artwork && info.key != local_art_key_) {
    local_art_key_ = info.key;
    local_cover_sha_.clear();
    if (!info.is_url) {
      LocalArt::Result art =
          ExtractLocalArt(info, settings.use_albumart && settings.local_cover);
      if (art.found) {
        local_art_found = true;
        local_cover_sha_ = art.sha256_hex;
        MaybePublishLocalCover(info, std::move(art));
        std::lock_guard<std::mutex> lock(presence_mutex_);
        local_published = !local_cover_url_.empty();
      }
    }
  }

  {
    std::lock_guard<std::mutex> lock(presence_mutex_);
    SetInfo(info);
    SetSmallImage(info);
    SetTimestamp(info);
  }

  track_key_ = info.key;
  track_artist_ = info.artist;
  track_album_ = info.album;
  track_file_ = info.file;

  if (request_artwork) {
    if (PresenceLayout::ShouldRequestOnlineForTrack(
            settings.use_albumart, settings.use_albumart_online, info.artist,
            local_art_found, local_published)) {
      album_art_.Request(info.artist, info.album, info.file, info.title);
      online_requested_ = true;
    } else {
      std::lock_guard<std::mutex> lock(presence_mutex_);
      artwork_key_.clear();
      artwork_url_.clear();
      online_requested_ = false;
    }
  }

  const bool cover_ready =
      last_large_image_ != PresenceLayout::kTransparentLargeImageUrl;
  const uint64_t now_ms = MonotonicMillisNow();
  const bool has_source = cover_ready || HasPendingCoverSource();
  const PresenceUpdateGate::Decision gate_decision =
      update_gate_.OnTrack(info.key, false, cover_ready, now_ms, has_source);
  if (gate_decision == PresenceUpdateGate::Decision::Hold) {
    return;
  }
  SendActivity();
}

void AimpDiscordPresence::SetInfo(const TrackInfo& info) {
  activity_.type = static_cast<int>(DiscordIpc::ActivityType::kListening);
  activity_.status_display_type = settings.status_display_type;
  activity_.name = Utils::ToString(settings.name);

  track_fields_.title = info.title;
  track_fields_.artist = info.artist;
  track_fields_.album = info.album;
  track_fields_.year = info.year;
  track_fields_.genre = info.genre;

  const PresenceLayout::TextFields fields = PresenceLayout::BuildTextFields(
      track_fields_, Utils::ToString(settings.details_template),
      Utils::ToString(settings.state_template),
      Utils::ToString(settings.large_text_template));
  activity_.details = fields.details;
  activity_.state = fields.state;
  activity_.large_text = fields.large_text;

  const std::string large_image = ResolveLargeImageLocked(
      AlbumArt::BuildAlbumKey(info.artist, info.album, info.file), nullptr);
  activity_.large_image = large_image;
  last_large_image_ = large_image;
}

void AimpDiscordPresence::SetSmallImage(const TrackInfo& info, int state) {
  Aimp::Player::Service::Player player;
  if (state == -1) {
    state = player.State();
  }

  PresenceLayout::SmallBadgeSettings badge_settings;
  badge_settings.use_play = settings.status.use_play;
  badge_settings.play_image = Utils::ToString(settings.status.play_image);
  badge_settings.play_text = Utils::ToString(settings.status.play_text);
  badge_settings.use_pause = settings.status.use_pause;
  badge_settings.pause_image = Utils::ToString(settings.status.pause_image);
  badge_settings.pause_text = Utils::ToString(settings.status.pause_text);
  badge_settings.use_radio = settings.status.use_radio;
  badge_settings.radio_image = Utils::ToString(settings.status.radio_image);
  badge_settings.radio_text = Utils::ToString(settings.status.radio_text);

  const PresenceLayout::SmallBadge badge =
      PresenceLayout::SelectSmallBadge(info.is_url, state, badge_settings);
  activity_.small_image = badge.image;
  activity_.small_text = badge.text;
}

void AimpDiscordPresence::SetTimestamp(const TrackInfo& info) {
  activity_.start_timestamp = 0;
  activity_.end_timestamp = 0;

  const int64_t now = UnixSecondsNow();

  if (paused_) {
    activity_.start_timestamp = now;
    return;
  }

  Aimp::Player::Service::Player player;
  const int position = static_cast<int>(round(player.Position()));
  if (settings.timestamp && !info.is_url) {
    const int duration = static_cast<int>(round(player.Duration()));
    activity_.start_timestamp = now - position;
    if (duration - position > 0) {
      activity_.end_timestamp = now + duration - position;
    }
  } else {
    activity_.start_timestamp = now - position;
  }
}

void AimpDiscordPresence::ApplyResolvedArtwork(const std::string& artist, const std::string& album,
                                               const std::string& file_path,
                                               const std::string& url) {
  std::lock_guard<std::mutex> lock(presence_mutex_);
  artwork_key_ = AlbumArt::BuildAlbumKey(artist, album, file_path);
  artwork_url_ = url;
}

bool AimpDiscordPresence::ApplyPendingArtwork() {
  {
    std::string hash;
    bool ok = false;
    std::string url;
    std::string reason;
    std::string detail;
    std::string host;
    {
      std::lock_guard<std::mutex> lock(cover_mutex_);
      if (cover_result_ready_) {
        hash = cover_result_hash_;
        ok = cover_result_ok_;
        url = cover_result_url_;
        reason = cover_result_reason_;
        detail = cover_result_detail_;
        host = cover_result_host_;
        cover_result_hash_.clear();
        cover_result_url_.clear();
        cover_result_reason_.clear();
        cover_result_detail_.clear();
        cover_result_host_.clear();
        cover_result_ready_ = false;
      }
    }

    if (!hash.empty()) {
      if (!ok) {
        LogCover("local-cover failed hash=" + hash +
                 " host=" + (host.empty() ? "unknown" : host) +
                 " reason=" + (reason.empty() ? "unknown" : reason) +
                 " detail=" + (detail.empty() ? "unknown" : detail));
      } else {
        local_cover_urls_[hash] = url;
        LogCover("local-cover published hash=" + hash +
                 " host=" + (host.empty() ? "unknown" : host) + " url=" + url +
                 " detail=" + (detail.empty() ? "unknown" : detail));
        if (hash == local_cover_sha_) {
          std::lock_guard<std::mutex> lock(presence_mutex_);
          local_cover_url_ = url;
          local_cover_key_ =
              AlbumArt::BuildAlbumKey(track_artist_, track_album_, track_file_);
        } else {
          LogCover("local-cover dropped hash=" + hash + " reason=stale-track");
        }
      }
    }
  }

  std::string applied_image;
  std::string applied_source;
  std::string applied_artist;
  std::string applied_album;
  {
    std::lock_guard<std::mutex> lock(presence_mutex_);

    const std::string album_key =
        AlbumArt::BuildAlbumKey(track_artist_, track_album_, track_file_);
    const std::string large_image = ResolveLargeImageLocked(album_key, &applied_source);
    if (large_image == last_large_image_) {
      return false;
    }

    last_large_image_ = large_image;
    activity_.large_image = large_image;
    applied_image = large_image;
    applied_artist = track_artist_;
    applied_album = track_album_;
  }

  if (applied_source == "online") {
    online_requested_ = false;
  }

  LogCover("large-image applied artist=\"" + applied_artist + "\" album=\"" +
           applied_album + "\" source=" + applied_source + " url=" + applied_image);
  return true;
}

bool AimpDiscordPresence::HasPendingCoverSource() {
  PresenceUpdateGate::CoverSourceState state;
  state.cover_ready =
      last_large_image_ != PresenceLayout::kTransparentLargeImageUrl;

  if (!local_cover_sha_.empty()) {
    std::lock_guard<std::mutex> lock(cover_mutex_);
    if (cover_has_request_ && cover_request_hash_ == local_cover_sha_) {
      state.local_pending = true;
    } else if (cover_inflight_hash_ == local_cover_sha_) {
      state.local_pending = true;
    } else if (cover_result_ready_ && cover_result_hash_ == local_cover_sha_) {
      state.local_result_pending = true;
    }
  }

  state.online_requested = online_requested_;
  if (online_requested_) {
    const std::string album_key =
        AlbumArt::BuildAlbumKey(track_artist_, track_album_, track_file_);
    std::lock_guard<std::mutex> lock(presence_mutex_);
    state.online_key_matches = (artwork_key_ == album_key);
    state.online_url_nonempty = !artwork_url_.empty();
  }

  return PresenceUpdateGate::CoverSourcePending(state);
}

std::string AimpDiscordPresence::ResolveLargeImageLocked(const std::string& album_key,
                                                         std::string* source) const {
  const std::string local_url = settings.use_albumart ? local_cover_url_ : std::string();
  const std::string online_url = settings.use_albumart ? artwork_url_ : std::string();
  return PresenceLayout::ResolveCardLargeImage(album_key, local_cover_key_, local_url,
                                               artwork_key_, online_url, source);
}

void AimpDiscordPresence::MaybePublishLocalCover(const TrackInfo& info,
                                                 LocalArt::Result art) {
  if (!settings.use_albumart || !settings.local_cover) {
    return;
  }
  if (!art.found || art.bytes.empty() || art.sha256_hex.empty()) {
    return;
  }

  const std::string hash = art.sha256_hex;
  const size_t size = art.bytes.size();

  const char* mime = nullptr;
  if (art.format == "PNG") {
    mime = "image/png";
  } else if (art.format == "JPEG") {
    mime = "image/jpeg";
  }
  if (mime == nullptr) {
    LogCover("local-cover skipped hash=" + hash + " format=" + art.format +
             " reason=unsupported-format");
    return;
  }

  if (size > kMaxCoverBytes) {
    LogCover("local-cover skipped hash=" + hash + " size=" + std::to_string(size) +
             " limit=" + std::to_string(kMaxCoverBytes) + " reason=too-large");
    return;
  }

  const auto known = local_cover_urls_.find(hash);
  if (known != local_cover_urls_.end()) {
    {
      std::lock_guard<std::mutex> lock(presence_mutex_);
      local_cover_url_ = known->second;
      local_cover_key_ = AlbumArt::BuildAlbumKey(info.artist, info.album, info.file);
    }
    LogCover("local-cover hit hash=" + hash + " size=" + std::to_string(size) +
             " url=" + known->second);
    return;
  }

  {
    std::lock_guard<std::mutex> lock(cover_mutex_);
    if (cover_inflight_hash_ == hash ||
        (cover_result_ready_ && cover_result_hash_ == hash)) {
      return;
    }
    cover_request_hash_ = hash;
    cover_request_mime_ = mime;
    cover_request_bytes_ = std::move(art.bytes);
    cover_inflight_hash_ = hash;
    cover_has_request_ = true;
  }
  cover_wake_.notify_one();

  LogCover("local-cover queued hash=" + hash + " size=" + std::to_string(size) +
           " mime=" + mime);
}

void AimpDiscordPresence::CoverWorkerMain() {
  for (;;) {
    std::string hash;
    std::string mime;
    std::vector<unsigned char> bytes;
    {
      std::unique_lock<std::mutex> lock(cover_mutex_);
      cover_wake_.wait(lock, [this] { return cover_stopping_ || cover_has_request_; });
      if (cover_stopping_) {
        return;
      }
      hash = std::move(cover_request_hash_);
      mime = std::move(cover_request_mime_);
      bytes = std::move(cover_request_bytes_);
      cover_request_hash_.clear();
      cover_request_mime_.clear();
      cover_request_bytes_.clear();
      cover_has_request_ = false;
    }

    const CoverPublisher::Result result = CoverPublisher::Publish(bytes, mime.c_str());
    std::vector<unsigned char>().swap(bytes);

    {
      std::lock_guard<std::mutex> lock(cover_mutex_);
      if (cover_inflight_hash_ == hash) {
        cover_inflight_hash_.clear();
      }
      cover_result_hash_ = hash;
      cover_result_ok_ = result.ok;
      cover_result_url_ = result.url;
      cover_result_reason_ = result.reason;
      cover_result_detail_ = result.detail;
      cover_result_host_ = result.host;
      cover_result_ready_ = true;
    }
  }
}

void AimpDiscordPresence::StopCoverWorker() {
  {
    std::lock_guard<std::mutex> lock(cover_mutex_);
    cover_stopping_ = true;
  }
  cover_wake_.notify_all();
  if (cover_worker_.joinable()) {
    cover_worker_.join();
  }
}

void AimpDiscordPresence::LogCover(const std::string& line) {
  if (client_) {
    client_->LogLine(line);
  }
}

LocalArt::Result AimpDiscordPresence::ExtractLocalArt(const TrackInfo& info,
                                                      bool want_bytes) {
  LocalArt::Result result;

  Aimp::Player::Service::Player player;
  const Aimp::FileManager::FileInfo file_info = player.GetInfo();
  IAIMPFileInfo* raw_info = file_info.get();

  const std::wstring file =
      file_info.Get<std::wstring>(Aimp::FileManager::FileInfo::Props::kFileName);
  if (raw_info == nullptr || file.empty()) {
    return result;
  }

  IAIMPServiceAlbumArt* service =
      Aimp::Detail::QueryService<IAIMPServiceAlbumArt>(IID_IAIMPServiceAlbumArt);
  if (service == nullptr) {
    return result;
  }

  const LocalArt::Result art = LocalArt::Extract(service, raw_info, want_bytes);
  service->Release();

  std::string online_url;
  {
    std::lock_guard<std::mutex> lock(presence_mutex_);
    online_url = artwork_url_;
  }

  std::string line = "local-art track=\"" + info.artist + " - " + info.title +
                     "\" album=\"" + info.album + "\" file=\"" + Utils::ToString(file) + "\"";
  LocalArt::Result sidecar_art;
  if (art.found) {
    line += " found=1 source=sdk size=" + std::to_string(art.size) + " sha256=" + art.sha256_hex +
            " format=" + art.format + " aimp_format=" + std::to_string(art.aimp_format) +
            " dims=" + std::to_string(art.width) + "x" + std::to_string(art.height);
    line += " provider=offline-only flags=WAITFOR|OFFLINE|ORIGINAL|NOCACHE";
  } else {
    const std::wstring sidecar = LocalArt::FindSidecarArt(file);
    if (!sidecar.empty() && want_bytes) {
      sidecar_art = LocalArt::LoadSidecarBytes(sidecar);
    }

    line += " found=" + std::to_string(sidecar_art.found ? 1 : 0);
    if (sidecar_art.found) {
      line += " source=sidecar path=\"" + Utils::ToString(sidecar) + "\"";
      line += " size=" + std::to_string(sidecar_art.size) + " sha256=" + sidecar_art.sha256_hex +
              " format=" + sidecar_art.format +
              " aimp_format=" + std::to_string(sidecar_art.aimp_format) +
              " dims=" + std::to_string(sidecar_art.width) + "x" +
              std::to_string(sidecar_art.height);
    } else {
      line += " source=sdk";
      if (!sidecar.empty()) {
        line += " sidecar_path=\"" + Utils::ToString(sidecar) + "\" sidecar_found=0";
      }
    }
    line += " provider=offline-only flags=WAITFOR|OFFLINE|ORIGINAL|NOCACHE";
  }
  line += " online_url=\"";
  line += online_url;
  line += "\"";

  LogCover(line);
  return art.found ? art : sidecar_art;
}

void AimpDiscordPresence::SendActivity() {
  DiscordIpc::Activity snapshot;
  {
    std::lock_guard<std::mutex> lock(presence_mutex_);
    snapshot = activity_;
  }

  snapshot.buttons[0].label = PresenceLayout::FormatTemplate(
      Utils::ToString(settings.button1_label), track_fields_, false);
  snapshot.buttons[0].url = PresenceLayout::FormatTemplate(
      Utils::ToString(settings.button1_url), track_fields_, true);
  snapshot.buttons[1].label = PresenceLayout::FormatTemplate(
      Utils::ToString(settings.button2_label), track_fields_, false);
  snapshot.buttons[1].url = PresenceLayout::FormatTemplate(
      Utils::ToString(settings.button2_url), track_fields_, true);

  Aimp::Player::Service::Player player;
  const double position = player.Position();
  const int64_t now_seconds = UnixSecondsNow();

  {
    std::lock_guard<std::mutex> lock(throttle_mutex_);
    desired_activity_ = std::move(snapshot);
    desired_position_ = position;
    desired_at_seconds_ = now_seconds;
    desired_valid_ = true;
    ++desired_revision_;
  }
  throttle_wake_.notify_one();
}

void AimpDiscordPresence::StartThrottleWorker() {
  {
    std::lock_guard<std::mutex> lock(throttle_mutex_);
    throttle_stopping_ = false;
    desired_valid_ = false;
    desired_activity_ = DiscordIpc::Activity();
    desired_revision_ = 0;
    last_sent_revision_ = 0;
    throttle_last_sent_ms_ = 0;
    desired_position_ = 0.0;
    desired_at_seconds_ = 0;
  }
  throttle_worker_ = std::thread(&AimpDiscordPresence::ThrottleWorkerMain, this);
}

void AimpDiscordPresence::StopThrottleWorker() {
  {
    std::lock_guard<std::mutex> lock(throttle_mutex_);
    throttle_stopping_ = true;
    ++desired_revision_;
  }
  throttle_wake_.notify_all();
  if (throttle_worker_.joinable()) {
    throttle_worker_.join();
  }
}

void AimpDiscordPresence::QueueClearLocked() {
  desired_valid_ = false;
  desired_activity_ = DiscordIpc::Activity();
  ++desired_revision_;
  throttle_last_sent_ms_ = 0;
}

void AimpDiscordPresence::ClearPresenceBeforeStop() {
  if (!throttle_worker_.joinable() || !client_) {
    return;
  }

  std::unique_lock<std::mutex> lock(throttle_mutex_);
  if (throttle_stopping_) {
    return;
  }

  QueueClearLocked();
  const uint64_t clear_revision = desired_revision_;
  throttle_wake_.notify_one();

  throttle_wake_.wait_for(
      lock, std::chrono::milliseconds(kPresenceClearOnUnloadTimeoutMs),
      [this, clear_revision] {
        return throttle_stopping_ || last_sent_revision_ >= clear_revision;
      });

  const bool published = last_sent_revision_ >= clear_revision;
  lock.unlock();
  if (!published) {
    LogCover("presence clear on unload timed out; relying on client shutdown");
  }
}

void AimpDiscordPresence::ThrottleWorkerMain() {
  std::unique_lock<std::mutex> lock(throttle_mutex_);
  for (;;) {
    if (throttle_stopping_) {
      return;
    }

    PresenceThrottlePlan::Request request;
    request.desired_valid = desired_valid_;
    request.desired_revision = desired_revision_;
    request.last_sent_revision = last_sent_revision_;
    request.last_sent_ms = throttle_last_sent_ms_;

    const uint32_t interval =
        PresenceRateLimit::NormalizeThrottleMs(settings.throttle_ms);
    const PresenceThrottlePlan::Decision decision = PresenceThrottlePlan::Decide(
        request, MonotonicMillisNow(), interval);

    if (decision.action == PresenceThrottlePlan::Action::kIdle) {
      throttle_wake_.wait(lock, [this] {
        return throttle_stopping_ || desired_revision_ != last_sent_revision_;
      });
      continue;
    }

    if (decision.action == PresenceThrottlePlan::Action::kWait) {
      const uint64_t observed_revision = desired_revision_;
      throttle_wake_.wait_for(
          lock, std::chrono::milliseconds(static_cast<int64_t>(decision.wait_ms)),
          [this, observed_revision] {
            return throttle_stopping_ || desired_revision_ != observed_revision;
          });
      continue;
    }

    const uint64_t acted_revision = desired_revision_;
    const bool clear =
        decision.action == PresenceThrottlePlan::Action::kClear;
    const uint64_t gate_before = throttle_last_sent_ms_;
    DiscordIpc::Activity snapshot;
    double position = 0.0;
    int64_t at_seconds = 0;
    if (!clear) {
      snapshot = desired_activity_;
      position = desired_position_;
      at_seconds = desired_at_seconds_;
    }

    lock.unlock();
    if (client_) {
      if (clear) {
        client_->Clear();
      } else {
        client_->SetActivity(snapshot);
      }
    }
    if (!clear) {
      sent_position_.store(position, std::memory_order_relaxed);
      sent_at_seconds_.store(at_seconds, std::memory_order_relaxed);
    } else {
      sent_at_seconds_.store(0, std::memory_order_relaxed);
    }
    lock.lock();

    last_sent_revision_ = acted_revision;
    throttle_last_sent_ms_ = PresenceThrottlePlan::GateAfterAction(
        decision.action, gate_before, throttle_last_sent_ms_,
        MonotonicMillisNow());
    throttle_wake_.notify_all();
  }
}
