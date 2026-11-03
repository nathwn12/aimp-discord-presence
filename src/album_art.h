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

#ifndef AIMPDISCORDPRESENCE_SRC_ALBUM_ART_H_
#define AIMPDISCORDPRESENCE_SRC_ALBUM_ART_H_

#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstdint>
#include <deque>
#include <functional>
#include <mutex>
#include <string>
#include <thread>
#include <unordered_map>

// Resolves album art URLs from keyless public APIs on a background worker so
// AIMP's message thread is never blocked on the network. The providers are
// tried in preference order:
//
//   1. Deezer album search (`cover_xl`).
//   2. iTunes Search API (`artworkUrl100`, upscaled to 600x600).
//   3. MusicBrainz release-group search for an MBID, then the Cover Art Archive
//      front cover for that MBID; on a miss the WS/2 release search is tried
//      with the Cover Art Archive `/release/<mbid>/front-500` cover.
//   4. Wikipedia pageimages (`generator=search`, `pithumbsize=600`,
//      `pilicense=any`) with an exact-title guard.
//   5. Discogs database search, then `GET /releases/<id>` for `images[0].uri`.
//
// The lookup runs in two tiers: Tier 1 (Deezer + iTunes) runs in parallel and
// wins whenever either hits; only when both miss does Tier 2 (MusicBrainz +
// Wikipedia + Discogs) run in parallel. Tiering keeps the common case to one
// round trip and keeps the enrichment hosts (Discogs 60 req/min unauth) off
// the hot path. Wall time is the slowest provider in the tier that ran, not
// the sum.
//
// No provider needs an API key. A hit is only accepted when a light identity
// guard confirms the hit really is the requested artist + album, because
// free-text search happily ranks an unrelated album first.
//
// An empty album tag is resolved first: Deezer track search with the strict
// `artist:"..." track:"..."` syntax and iTunes `entity=song` are queried for
// the track, guarded by artist + title, and the returned album
// (`album.title` / `collectionName`) becomes the effective album for the art
// lookup. When resolution fails the lookup falls back to the artist-only
// search.
//
// The resolved URL is handed to a callback that runs on the worker thread; the
// callback must not call AIMP services, it should only publish the value to
// the plugin which applies it on the next player notification.
namespace AlbumArt {

// Discord accepts external asset URLs up to 300 characters.
constexpr size_t kMaxUrlLength = 300;

// MusicBrainz policy requires a descriptive User-Agent with a contact URL.
constexpr const char* kUserAgent =
    "AIMP-Discord-Presence/2.2.0 (https://github.com/nathwn12/aimp-discord-presence)";

enum class Provider {
  kNone = 0,
  kDeezer,
  kItunes,
  kMusicBrainz,
  kMusicBrainzRelease,
  kWikipedia,
  kDiscogs,
};

// --- Pure helpers, unit tested separately ---------------------------------

// Builds the identity key used both as the resolver cache key and to match a
// resolved artwork URL with the track currently playing. An album tag is the
// whole identity (artist + "\n" + album) and the track title is deliberately
// ignored: artwork is per-album, and one shared builder keeps the resolver and
// the plugin from drifting apart. An album-less track has no album identity, so
// the file path captured at extraction time is appended instead
// (artist + "\n\n" + file_path); without that discriminator every untagged file
// by one artist would share a key and inherit another file's cover.
std::string BuildAlbumKey(const std::string& artist, const std::string& album,
                          const std::string& file_path);

// Percent-encodes a UTF-8 string for use as a query parameter value.
std::string UriEncodeUtf8(const std::string& utf8);

// Returns the value of the first `"key":"value"` pair in a JSON document,
// with JSON escapes decoded. Returns an empty string when the key is absent.
std::string ExtractJsonStringField(const std::string& json, const std::string& key);

// Rewrites an iTunes artwork URL (`.../100x100bb.jpg`) to its 600x600 variant,
// which is the largest size iTunes serves for every artwork.
std::string UpscaleItunesArtwork(const std::string& url);

std::string BuildDeezerSearchUrl(const std::string& artist, const std::string& album);
// Deezer track search with the strict `artist:"..." track:"..."` syntax, used
// only to resolve an album title for an empty album tag. Returns an empty
// string when artist or track is empty.
std::string BuildDeezerTrackSearchUrl(const std::string& artist,
                                      const std::string& track);
std::string BuildItunesSearchUrl(const std::string& artist, const std::string& album);
// iTunes song search (`entity=song`), used only to resolve an album title for
// an empty album tag. Returns an empty string when artist or track is empty.
std::string BuildItunesSongSearchUrl(const std::string& artist,
                                     const std::string& track);
// MusicBrainz WS/2 release-group search. Requested at most once per second.
std::string BuildMusicBrainzReleaseGroupUrl(const std::string& artist,
                                            const std::string& album);
// MusicBrainz WS/2 release search (fallback when the release-group has no
// cover). Requested at most once per second, after the release-group attempt.
std::string BuildMusicBrainzReleaseSearchUrl(const std::string& artist,
                                             const std::string& album);
// Cover Art Archive cover for an MBID. Returns an empty string when `mbid` is
// not a UUID, so a malformed search response cannot inject a path.
std::string BuildCoverArtArchiveUrl(const std::string& mbid);
// Cover Art Archive cover for a release MBID (`/release/<mbid>/front-500`).
// Returns an empty string when `mbid` is not a UUID.
std::string BuildCoverArtArchiveReleaseUrl(const std::string& mbid);
// Discogs database search (keyless metadata; search rows carry no images, so a
// second `GET /releases/<id>` follows). Returns an empty string when there is
// nothing to query.
std::string BuildDiscogsSearchUrl(const std::string& artist, const std::string& album);
// Discogs release detail for `release_id`. Returns an empty string when the id
// is not all digits, so a malformed search response cannot inject a path.
std::string BuildDiscogsReleaseUrl(const std::string& release_id);
// Wikipedia pageimages with `generator=search` (keyless fallback thumbnails).
// Returns an empty string when there is nothing to query.
std::string BuildWikipediaSearchUrl(const std::string& artist, const std::string& album);

// Extracts the artwork URL for a provider from a search response. Returns an
// empty string when no usable (https, <= 300 characters) URL is present, and
// for providers (MusicBrainz) whose answer is not an image URL.
std::string ExtractArtworkUrl(const std::string& body, Provider provider);

// Extracts the MBID of the first release group in a MusicBrainz WS/2 search
// response.
std::string ExtractMusicBrainzReleaseGroupId(const std::string& body);
// Extracts the MBID of the first release in a MusicBrainz WS/2 release search
// response.
std::string ExtractMusicBrainzReleaseId(const std::string& body);
// Extracts the numeric release id of the first Discogs database search hit.
// Returns an empty string when no hit carries one.
std::string ExtractDiscogsReleaseId(const std::string& body);
// Extracts the album title of a Deezer track-search hit (`album.title`).
std::string ExtractDeezerTrackAlbumTitle(const std::string& body);
// Extracts the album title of an iTunes song-search hit (`collectionName`).
std::string ExtractItunesAlbumTitle(const std::string& body);
// Extracts the title of the first Wikipedia pageimages page.
std::string ExtractWikipediaPageTitle(const std::string& body);
// Extracts the thumbnail URL of the first Wikipedia pageimages page. Returns
// an empty string when the page carries no usable (https, <= 300) thumbnail.
std::string ExtractWikipediaThumbnailUrl(const std::string& body);
// Extracts the first usable cover URL (`images[].uri`) from a Discogs release
// detail document.
std::string ExtractDiscogsImageUrl(const std::string& body);

// True when a provider hit's identity agrees with the requested artist/album,
// so a guard check can be exercised without a provider-specific response
// document. Agreement ignores case, punctuation and whitespace, and accepts a
// word-prefix on either side ("Discovery" matches "Discovery (Deluxe Edition)",
// but "Program Music I" does not match "Program Music III").
//
// An empty `expected` never matches: a genuinely empty candidate is always
// rejected. When `expected` is non-empty it must agree with `candidate`. The
// album half is the same rule, with one deliberate exception: an empty
// *requested* album means the lookup was artist-only, so the album comparison
// is skipped and the artist agreement alone admits the hit. A requested album
// that is present is still compared strictly.
bool MetadataMatches(const std::string& expected, const std::string& candidate);

// Light sanity check on a free-text search response: true only when the first
// hit's own artist and title agree with the requested pair (see
// MetadataMatches). A rejected hit falls through to the next provider.
// When `album` is empty the search is artist-only, so the hit's album is not
// compared and any album it names is accepted; the hit still has to name a
// non-empty album for the provider extractor to consider it a usable hit.
// Wikipedia compares the page title against the requested album (or artist when
// the album is empty); Discogs compares its `title` ("Artist - Album") against
// `artist + " " + album` (or the artist alone for an artist-only lookup).
bool SearchResultMatches(const std::string& body, Provider provider,
                         const std::string& artist, const std::string& album);

// Track-level guards for album-title resolution: true only when the first hit
// names the requested artist AND track title, and carries a non-empty album
// title for the caller to adopt.
bool DeezerTrackResultMatches(const std::string& body, const std::string& artist,
                              const std::string& track);
bool ItunesSongResultMatches(const std::string& body, const std::string& artist,
                             const std::string& track);

// --- Background resolver ---------------------------------------------------

class Resolver {
 public:
  // `file_path` is the caller's identity discriminator (see BuildAlbumKey),
  // echoed back so the caller rebuilds the exact key it requested with.
  // `track` is the track title (UTF-8); when `album` is empty it is used to
  // resolve the album title before the art lookup runs.
  using Callback = std::function<void(const std::string& artist,
                                      const std::string& album,
                                      const std::string& file_path,
                                      const std::string& url)>;

  Resolver();
  ~Resolver();

  Resolver(const Resolver&) = delete;
  Resolver& operator=(const Resolver&) = delete;

  // When disabled no network request is ever made; requests resolve to "".
  void SetOnlineEnabled(bool enabled);

  void SetCallback(Callback callback);

  // Diagnostics: one concise line per online lookup decision (requested,
  // resolved, not-found); never image bytes. Invoked on the worker thread, so
  // the callback must itself be thread-safe.
  using Logger = std::function<void(const std::string&)>;
  void SetLogger(Logger logger);

  // Queues a lookup for `artist` + `album` (UTF-8). `file_path` is the opaque
  // identity discriminator that keys the cache and is echoed to the callback;
  // `track` is the track title used only to resolve an empty `album`.
  // Replaces any pending request; stale completions are dropped.
  void Request(const std::string& artist, const std::string& album,
               const std::string& file_path, const std::string& track = std::string());

  // Blocking lookup (network + cache). Exposed for the worker and tests.
  // `track` resolves an empty `album` before the art lookup runs.
  std::string Lookup(const std::string& artist, const std::string& album,
                     const std::string& file_path,
                     const std::string& track = std::string());

  // Blocking single-provider lookup (network, no cache). Exposed so offline
  // tests pin the pure helpers and live tests prove each provider on the wire.
  // `answered` is true when the provider gave a definitive answer (even a
  // miss) and false on a transport failure, which the caller must not cache
  // as "no artwork". May be nullptr when the caller does not need it.
  std::string LookupFromProvider(Provider provider, const std::string& artist,
                                 const std::string& album, bool* answered);

  // Resolves an album title for an empty album tag from the track title, via
  // Deezer strict track search then iTunes `entity=song`, each guarded by
  // artist + title. Returns "" when neither resolves.
  std::string ResolveAlbumTitle(const std::string& artist, const std::string& track);

  void Shutdown();

 private:
  void WorkerMain();
  std::string LookupUncached(const std::string& artist, const std::string& album,
                             const std::string& track, bool* definitive);
  // Online-rung diagnostics; called on the worker thread.
  void Log(const std::string& line);
  void LogOutcome(const std::string& artist, const std::string& album,
                  const std::string& url);
  // MusicBrainz allows one request per second; Discogs allows 60 requests per
  // minute unauthenticated, so it gets the same 1 req/s gate. Both gates are
  // thread-safe: Tier 2 runs its providers on concurrent threads.
  void WaitForMusicBrainzSlot();
  void WaitForDiscogsSlot();
  bool TakeCached(const std::string& key, std::string* url);
  void StoreCached(const std::string& key, const std::string& url);

  std::thread worker_;

  std::mutex mutex_;
  std::condition_variable wake_;
  // Set by Shutdown() and polled by the worker between provider requests so an
  // in-flight resolve aborts promptly instead of running the full timeout
  // budget while AIMP unloads.
  std::atomic<bool> stopping_{false};
  bool online_ = true;

  bool has_request_ = false;
  uint64_t generation_ = 0;
  std::string request_artist_;
  std::string request_album_;
  std::string request_file_path_;
  std::string request_track_;

  Callback callback_;
  Logger logger_;

  std::unordered_map<std::string, std::string> cache_;
  std::deque<std::string> cache_order_;

  // Rate-gate state. Guarded by gate_mutex_ (not mutex_) so Tier 2 threads and
  // live tests share it safely.
  std::mutex gate_mutex_;
  std::chrono::steady_clock::time_point last_musicbrainz_request_{};
  std::chrono::steady_clock::time_point last_discogs_request_{};

  static constexpr size_t kMaxCacheEntries = 200;
};

}  // namespace AlbumArt

#endif  // AIMPDISCORDPRESENCE_SRC_ALBUM_ART_H_
