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

#include "album_art.h"

#include <windows.h>
#include <winhttp.h>

#include <algorithm>
#include <cctype>
#include <chrono>
#include <future>
#include <thread>
#include <utility>
#include <vector>

namespace AlbumArt {
namespace {

constexpr size_t kMaxResponseBytes = 512 * 1024;
constexpr int kTimeoutMilliseconds = 1500;

constexpr const char* kDeezerHost = "api.deezer.com";
constexpr const char* kItunesHost = "itunes.apple.com";
constexpr const char* kMusicBrainzHost = "musicbrainz.org";
constexpr const char* kCoverArtArchiveHost = "coverartarchive.org";
constexpr const char* kDiscogsHost = "api.discogs.com";
constexpr const char* kWikipediaHost = "en.wikipedia.org";

// MusicBrainz allows one request per second per client; the gate is set a
// little above that so clock granularity cannot drift it below the limit.
// Discogs allows 60 requests per minute unauthenticated, so it gets the same
// gate and Tier 2 never spends that budget on the hot path (Tier 1 runs
// first and Tier 2 only runs when both Tier 1 providers miss).
constexpr int kMusicBrainzIntervalMilliseconds = 1100;
constexpr int kDiscogsIntervalMilliseconds = 1100;

constexpr const char* kHexDigits = "0123456789ABCDEF";

bool IsUnreserved(unsigned char value) {
  if ((value >= 'A' && value <= 'Z') || (value >= 'a' && value <= 'z') ||
      (value >= '0' && value <= '9')) {
    return true;
  }
  return value == '-' || value == '_' || value == '.' || value == '~';
}

std::wstring WidenUtf8(const std::string& utf8) {
  if (utf8.empty()) {
    return std::wstring();
  }
  const int length = MultiByteToWideChar(CP_UTF8, 0, utf8.c_str(),
                                         static_cast<int>(utf8.size()), nullptr, 0);
  if (length <= 0) {
    return std::wstring();
  }
  std::wstring wide(static_cast<size_t>(length), L'\0');
  MultiByteToWideChar(CP_UTF8, 0, utf8.c_str(), static_cast<int>(utf8.size()), wide.data(),
                      length);
  return wide;
}

void AppendUtf8Codepoint(std::string& output, unsigned int codepoint) {
  if (codepoint <= 0x7F) {
    output.push_back(static_cast<char>(codepoint));
  } else if (codepoint <= 0x7FF) {
    output.push_back(static_cast<char>(0xC0 | (codepoint >> 6)));
    output.push_back(static_cast<char>(0x80 | (codepoint & 0x3F)));
  } else if (codepoint <= 0xFFFF) {
    output.push_back(static_cast<char>(0xE0 | (codepoint >> 12)));
    output.push_back(static_cast<char>(0x80 | ((codepoint >> 6) & 0x3F)));
    output.push_back(static_cast<char>(0x80 | (codepoint & 0x3F)));
  } else {
    output.push_back(static_cast<char>(0xF0 | (codepoint >> 18)));
    output.push_back(static_cast<char>(0x80 | ((codepoint >> 12) & 0x3F)));
    output.push_back(static_cast<char>(0x80 | ((codepoint >> 6) & 0x3F)));
    output.push_back(static_cast<char>(0x80 | (codepoint & 0x3F)));
  }
}

int HexValue(char value) {
  if (value >= '0' && value <= '9') {
    return value - '0';
  }
  if (value >= 'a' && value <= 'f') {
    return value - 'a' + 10;
  }
  if (value >= 'A' && value <= 'F') {
    return value - 'A' + 10;
  }
  return -1;
}

// Reads a JSON string literal that starts right after the opening quote and
// decodes its escapes.
std::string DecodeJsonStringAt(const std::string& json, size_t position) {
  std::string decoded;

  for (size_t i = position; i < json.size(); ++i) {
    const char current = json[i];
    if (current == '"') {
      break;
    }
    if (current != '\\') {
      decoded.push_back(current);
      continue;
    }

    if (i + 1 >= json.size()) {
      break;
    }
    const char escape = json[++i];
    switch (escape) {
      case '"':
      case '\\':
      case '/':
        decoded.push_back(escape);
        break;
      case 'b':
        decoded.push_back('\b');
        break;
      case 'f':
        decoded.push_back('\f');
        break;
      case 'n':
        decoded.push_back('\n');
        break;
      case 'r':
        decoded.push_back('\r');
        break;
      case 't':
        decoded.push_back('\t');
        break;
      case 'u': {
        if (i + 4 >= json.size()) {
          break;
        }
        unsigned int codepoint = 0;
        bool valid = true;
        for (size_t digit = 0; digit < 4; ++digit) {
          const int value = HexValue(json[i + 1 + digit]);
          if (value < 0) {
            valid = false;
            break;
          }
          codepoint = (codepoint << 4) | static_cast<unsigned int>(value);
        }
        if (valid) {
          AppendUtf8Codepoint(decoded, codepoint);
        }
        i += 4;
        break;
      }
      default:
        decoded.push_back(escape);
        break;
    }
  }

  return decoded;
}

// Returns the first non-empty value for `key` that is an https URL within
// Discord's length limit.
std::string ExtractFirstUsableUrl(const std::string& body, const std::string& key) {
  const std::string needle = "\"" + key + "\"";
  size_t search_from = 0;

  while (search_from < body.size()) {
    const size_t position = body.find(needle, search_from);
    if (position == std::string::npos) {
      return std::string();
    }
    search_from = position + needle.size();

    size_t cursor = search_from;
    while (cursor < body.size() && std::isspace(static_cast<unsigned char>(body[cursor])) != 0) {
      ++cursor;
    }
    if (cursor >= body.size() || body[cursor] != ':') {
      continue;
    }
    ++cursor;
    while (cursor < body.size() && std::isspace(static_cast<unsigned char>(body[cursor])) != 0) {
      ++cursor;
    }
    if (cursor >= body.size() || body[cursor] != '"') {
      continue;
    }

    const std::string value = DecodeJsonStringAt(body, cursor + 1);
    if (value.size() >= 8 && value.compare(0, 8, "https://") == 0 &&
        value.size() <= kMaxUrlLength) {
      return value;
    }
  }

  return std::string();
}

class WinHttpHandle {
 public:
  explicit WinHttpHandle(HINTERNET handle = nullptr) : handle_(handle) {}
  ~WinHttpHandle() {
    if (handle_ != nullptr) {
      WinHttpCloseHandle(handle_);
    }
  }

  WinHttpHandle(const WinHttpHandle&) = delete;
  WinHttpHandle& operator=(const WinHttpHandle&) = delete;

  HINTERNET get() const { return handle_; }
  bool valid() const { return handle_ != nullptr; }

 private:
  HINTERNET handle_;
};

// Fetches `url` and stores the response body in `body`. Returns true only when
// a complete HTTP 200 response was received, so the caller can tell a
// definitive provider answer (even an empty one) apart from a transport
// failure, which must not be cached as "no artwork". Redirects are followed by
// WinHTTP's default policy (https only), which the Cover Art Archive needs: its
// size endpoints answer 307 toward the Internet Archive copy of the image.
//
// `user_agent` is sent as an additional header when non-empty; `status_code`
// receives the HTTP status code, or 0 when no response was received at all, so
// a caller can tell a 404 from a timeout.
bool HttpGet(const std::string& url, std::string* body,
             const std::string& user_agent = std::string(),
             int* status_code = nullptr) {
  body->clear();
  if (status_code != nullptr) {
    *status_code = 0;
  }

  const std::wstring wide_url = WidenUtf8(url);
  if (wide_url.empty()) {
    return false;
  }

  URL_COMPONENTS components = {};
  components.dwStructSize = sizeof(components);
  wchar_t host[256] = {};
  wchar_t path[2048] = {};
  components.lpszHostName = host;
  components.dwHostNameLength = static_cast<DWORD>(std::size(host));
  components.lpszUrlPath = path;
  components.dwUrlPathLength = static_cast<DWORD>(std::size(path));

  if (!WinHttpCrackUrl(wide_url.c_str(), 0, 0, &components)) {
    return false;
  }

  WinHttpHandle session(WinHttpOpen(L"AIMP-Discord-Presence/2.0",
                                    WINHTTP_ACCESS_TYPE_DEFAULT_PROXY,
                                    WINHTTP_NO_PROXY_NAME, WINHTTP_NO_PROXY_BYPASS, 0));
  if (!session.valid()) {
    return false;
  }
  WinHttpSetTimeouts(session.get(), kTimeoutMilliseconds, kTimeoutMilliseconds,
                     kTimeoutMilliseconds, kTimeoutMilliseconds);

  WinHttpHandle connection(WinHttpConnect(session.get(), host, components.nPort, 0));
  if (!connection.valid()) {
    return false;
  }

  const DWORD flags = components.nScheme == INTERNET_SCHEME_HTTPS ? WINHTTP_FLAG_SECURE : 0;
  WinHttpHandle request(WinHttpOpenRequest(connection.get(), L"GET", path, nullptr,
                                           WINHTTP_NO_REFERER, WINHTTP_DEFAULT_ACCEPT_TYPES,
                                           flags));
  if (!request.valid()) {
    return false;
  }

  // A descriptive User-Agent is mandatory for MusicBrainz and Discogs; the
  // other providers use the session User-Agent.
  std::wstring header_block;
  if (!user_agent.empty()) {
    header_block = L"User-Agent: ";
    header_block += WidenUtf8(user_agent);
    header_block += L"\r\n";
  }
  if (!WinHttpSendRequest(request.get(),
                          header_block.empty() ? WINHTTP_NO_ADDITIONAL_HEADERS
                                               : header_block.c_str(),
                          header_block.empty()
                              ? 0
                              : static_cast<DWORD>(header_block.size()),
                          WINHTTP_NO_REQUEST_DATA, 0, 0, 0) ||
      !WinHttpReceiveResponse(request.get(), nullptr)) {
    return false;
  }

  DWORD status = 0;
  DWORD status_size = sizeof(status);
  if (!WinHttpQueryHeaders(request.get(),
                           WINHTTP_QUERY_STATUS_CODE | WINHTTP_QUERY_FLAG_NUMBER,
                           WINHTTP_HEADER_NAME_BY_INDEX, &status, &status_size,
                           WINHTTP_NO_HEADER_INDEX)) {
    return false;
  }
  if (status_code != nullptr) {
    *status_code = static_cast<int>(status);
  }
  if (status != 200) {
    return false;
  }

  std::string result;
  bool complete = false;
  for (;;) {
    DWORD available = 0;
    if (!WinHttpQueryDataAvailable(request.get(), &available)) {
      break;
    }
    if (available == 0) {
      complete = true;
      break;
    }

    const DWORD chunk = (std::min)(
        available, static_cast<DWORD>(kMaxResponseBytes - result.size()));
    std::string buffer(static_cast<size_t>(chunk), '\0');
    DWORD read = 0;
    if (!WinHttpReadData(request.get(), buffer.data(), chunk, &read) || read == 0) {
      break;
    }
    result.append(buffer, 0, static_cast<size_t>(read));
    if (result.size() >= kMaxResponseBytes) {
      // The artwork URL always appears early in these documents; a capped
      // response is still a usable, definitive answer.
      complete = true;
      break;
    }
  }

  if (!complete) {
    return false;
  }

  *body = std::move(result);
  return true;
}

}  // namespace

std::string BuildAlbumKey(const std::string& artist, const std::string& album,
                          const std::string& file_path) {
  if (!album.empty()) {
    return artist + "\n" + album;
  }
  // Album-less: artist + "\n" + album alone would collapse every untagged file
  // by one artist into a single identity, so the file path discriminates.
  return artist + "\n\n" + file_path;
}

std::string UriEncodeUtf8(const std::string& utf8) {
  std::string encoded;
  encoded.reserve(utf8.size() * 3);

  for (const char raw : utf8) {
    const unsigned char value = static_cast<unsigned char>(raw);
    if (IsUnreserved(value)) {
      encoded.push_back(static_cast<char>(value));
    } else {
      encoded.push_back('%');
      encoded.push_back(kHexDigits[(value >> 4) & 0x0F]);
      encoded.push_back(kHexDigits[value & 0x0F]);
    }
  }

  return encoded;
}

std::string ExtractJsonStringField(const std::string& json, const std::string& key) {
  const size_t position = json.find("\"" + key + "\"");
  if (position == std::string::npos) {
    return std::string();
  }

  size_t cursor = position + key.size() + 2;
  while (cursor < json.size() && std::isspace(static_cast<unsigned char>(json[cursor])) != 0) {
    ++cursor;
  }
  if (cursor >= json.size() || json[cursor] != ':') {
    return std::string();
  }
  ++cursor;
  while (cursor < json.size() && std::isspace(static_cast<unsigned char>(json[cursor])) != 0) {
    ++cursor;
  }
  if (cursor >= json.size() || json[cursor] != '"') {
    return std::string();
  }

  return DecodeJsonStringAt(json, cursor + 1);
}

std::string UpscaleItunesArtwork(const std::string& url) {
  const size_t marker = url.rfind("bb.");
  if (marker == std::string::npos) {
    return url;
  }

  // Walk back over "<width>x<height>" to find the start of the size token.
  size_t start = marker;
  while (start > 0) {
    const char previous = url[start - 1];
    if (std::isdigit(static_cast<unsigned char>(previous)) != 0 || previous == 'x') {
      --start;
    } else {
      break;
    }
  }

  if (start == marker || url[start] == 'x') {
    return url;
  }

  return url.substr(0, start) + "600x600bb." + url.substr(marker + 3);
}

namespace {

// Normalizes a metadata string for the identity guard: ASCII letters are
// lowercased and every run of other characters collapses to one space.
// Non-ASCII bytes count as separators, so a guard check can only ever reject a
// hit; it never accepts a wrong one on a technicality.
std::string NormalizeForMatch(const std::string& text) {
  std::string normalized;
  normalized.reserve(text.size());
  bool separator_pending = false;

  for (const char raw : text) {
    const unsigned char value = static_cast<unsigned char>(raw);
    const bool is_ascii_alnum = (value >= '0' && value <= '9') ||
                                (value >= 'a' && value <= 'z') ||
                                (value >= 'A' && value <= 'Z');
    if (!is_ascii_alnum) {
      separator_pending = !normalized.empty();
      continue;
    }
    if (separator_pending) {
      normalized.push_back(' ');
      separator_pending = false;
    }
    normalized.push_back(static_cast<char>(std::tolower(value)));
  }

  return normalized;
}

std::vector<std::string> SplitWords(const std::string& normalized) {
  std::vector<std::string> words;
  size_t start = 0;
  while (start < normalized.size()) {
    const size_t end = normalized.find(' ', start);
    if (end == std::string::npos) {
      words.push_back(normalized.substr(start));
      break;
    }
    words.push_back(normalized.substr(start, end - start));
    start = end + 1;
  }
  return words;
}

// Reads the first hit's artist and album out of a search response. Only the
// response shapes of the providers that are actually queried are handled; false
// means the response carried no usable hit identity.
bool ExtractSearchHitIdentity(const std::string& body, Provider provider,
                              std::string* hit_artist, std::string* hit_album) {
  if (body.empty()) {
    return false;
  }

  if (provider == Provider::kDeezer) {
    // {"data":[{"title":"...","artist":{"name":"..."}},...]}
    *hit_album = ExtractJsonStringField(body, "title");
    const size_t artist_key = body.find("\"artist\"");
    *hit_artist = artist_key == std::string::npos
                      ? std::string()
                      : ExtractJsonStringField(body.substr(artist_key), "name");
    return !hit_artist->empty() && !hit_album->empty();
  }

  if (provider == Provider::kItunes) {
    // {"results":[{"artistName":"...","collectionName":"..."},...]}
    *hit_artist = ExtractJsonStringField(body, "artistName");
    *hit_album = ExtractJsonStringField(body, "collectionName");
    return !hit_artist->empty() && !hit_album->empty();
  }

  if (provider == Provider::kMusicBrainz) {
    // Everything is read after the "release-groups" key: the document also
    // nests releases[].id and artist-credit[].artist.id, so an "id" or "name"
    // from the top of the document could belong to the wrong object.
    const size_t anchor = body.find("\"release-groups\"");
    if (anchor == std::string::npos) {
      return false;
    }
    const std::string release_group = body.substr(anchor);
    *hit_album = ExtractJsonStringField(release_group, "title");
    const size_t credit = release_group.find("\"artist-credit\"");
    *hit_artist = credit == std::string::npos
                      ? std::string()
                      : ExtractJsonStringField(release_group.substr(credit), "name");
    return !hit_artist->empty() && !hit_album->empty();
  }

  if (provider == Provider::kMusicBrainzRelease) {
    // {"releases":[{"title":"...","artist-credit":[{"name":"..."}]},...]}
    const size_t anchor = body.find("\"releases\"");
    if (anchor == std::string::npos) {
      return false;
    }
    const std::string release = body.substr(anchor);
    *hit_album = ExtractJsonStringField(release, "title");
    const size_t credit = release.find("\"artist-credit\"");
    *hit_artist = credit == std::string::npos
                      ? std::string()
                      : ExtractJsonStringField(release.substr(credit), "name");
    return !hit_artist->empty() && !hit_album->empty();
  }

  return false;
}

// A strict 8-4-4-4-12 hexadecimal UUID check, so a malformed MBID cannot inject
// a path segment into the Cover Art Archive URL.
bool IsUuid(const std::string& value) {
  if (value.size() != 36) {
    return false;
  }
  for (size_t index = 0; index < value.size(); ++index) {
    const bool is_separator = index == 8 || index == 13 || index == 18 || index == 23;
    if (is_separator) {
      if (value[index] != '-') {
        return false;
      }
      continue;
    }
    if (HexValue(value[index]) < 0) {
      return false;
    }
  }
  return true;
}

bool IsAllDigits(const std::string& value) {
  if (value.empty()) {
    return false;
  }
  for (const char raw : value) {
    if (std::isdigit(static_cast<unsigned char>(raw)) == 0) {
      return false;
    }
  }
  return true;
}

}  // namespace

std::string BuildDeezerSearchUrl(const std::string& artist, const std::string& album) {
  // Deezer retired the `artist:"..." album:"..."` advanced syntax: it now
  // answers 200 with {"data":[],"total":0} (verified 2026-10-01). The plain
  // free-text query returns the same album objects, including `cover_xl`, so
  // the album endpoint is queried with free text and the light identity guard
  // rejects the occasional wrong first hit.
  // An album-less track has nothing to append, so the free-text query is the
  // artist alone (with no trailing space).
  const std::string terms = album.empty() ? artist : artist + " " + album;
  std::string url = "https://";
  url += kDeezerHost;
  url += "/search/album?q=";
  url += UriEncodeUtf8(terms);
  url += "&limit=1";
  return url;
}

std::string BuildDeezerTrackSearchUrl(const std::string& artist,
                                      const std::string& track) {
  // Strict field syntax for album-title resolution (verified 2026-11-04 in the
  // keyless notes: plain `q=` ranking mis-ranks, e.g. Teardrop -> Sandstorm).
  if (artist.empty() || track.empty()) {
    return std::string();
  }
  const std::string query = "artist:\"" + artist + "\" track:\"" + track + "\"";
  std::string url = "https://";
  url += kDeezerHost;
  url += "/search/track?q=";
  url += UriEncodeUtf8(query);
  url += "&limit=1";
  return url;
}

std::string BuildItunesSearchUrl(const std::string& artist, const std::string& album) {
  // An album-less track has nothing to append, so the free-text term is the
  // artist alone (with no trailing space).
  const std::string terms = album.empty() ? artist : artist + " " + album;
  std::string url = "https://";
  url += kItunesHost;
  url += "/search?term=";
  url += UriEncodeUtf8(terms);
  url += "&media=music&limit=1";
  return url;
}

std::string BuildItunesSongSearchUrl(const std::string& artist,
                                     const std::string& track) {
  // Song search for album-title resolution (verified 2026-11-04: `entity=song`
  // carries `trackName` + `collectionName`; album-only search mis-ranks, e.g.
  // DSOTM -> a K-pop EP).
  if (artist.empty() || track.empty()) {
    return std::string();
  }
  const std::string terms = artist + " " + track;
  std::string url = "https://";
  url += kItunesHost;
  url += "/search?term=";
  url += UriEncodeUtf8(terms);
  url += "&entity=song&limit=1";
  return url;
}

std::string BuildMusicBrainzReleaseGroupUrl(const std::string& artist,
                                            const std::string& album) {
  // WS/2 search syntax ("artist:"..." AND releasegroup:"..."). The whole Lucene
  // query is percent-encoded, so the %22 sequences carry the phrase quotes;
  // backslashes and quotes inside a term are escaped first so a tag cannot
  // smuggle query syntax into the request.
  const auto escape_term = [](const std::string& term) {
    std::string escaped;
    escaped.reserve(term.size());
    for (const char current : term) {
      if (current == '\\' || current == '"') {
        escaped.push_back('\\');
      }
      escaped.push_back(current);
    }
    return escaped;
  };

  // An album-less track has no release group to name, so the releasegroup
  // clause is omitted and the artist clause alone is searched.
  const std::string query =
      album.empty() ? "artist:\"" + escape_term(artist) + "\""
                    : "artist:\"" + escape_term(artist) + "\" AND releasegroup:\"" +
                          escape_term(album) + "\"";
  std::string url = "https://";
  url += kMusicBrainzHost;
  url += "/ws/2/release-group/?query=";
  url += UriEncodeUtf8(query);
  url += "&fmt=json&limit=1";
  return url;
}

std::string BuildMusicBrainzReleaseSearchUrl(const std::string& artist,
                                             const std::string& album) {
  // WS/2 release search fallback (verified 2026-11-04:
  // `/ws/2/release/?query=release:"..." AND artist:"..."&fmt=json` returns the
  // release MBID whose cover lives at `/release/<mbid>/front-500`).
  const auto escape_term = [](const std::string& term) {
    std::string escaped;
    escaped.reserve(term.size());
    for (const char current : term) {
      if (current == '\\' || current == '"') {
        escaped.push_back('\\');
      }
      escaped.push_back(current);
    }
    return escaped;
  };

  const std::string query =
      album.empty() ? "artist:\"" + escape_term(artist) + "\""
                    : "release:\"" + escape_term(album) + "\" AND artist:\"" +
                          escape_term(artist) + "\"";
  std::string url = "https://";
  url += kMusicBrainzHost;
  url += "/ws/2/release/?query=";
  url += UriEncodeUtf8(query);
  url += "&fmt=json&limit=1";
  return url;
}

std::string BuildCoverArtArchiveUrl(const std::string& mbid) {
  if (!IsUuid(mbid)) {
    return std::string();
  }
  // The direct size endpoint answers with the image itself (after a redirect to
  // the Internet Archive copy), so there is no JSON body whose http:// image
  // URLs would have to be rewritten to https://.
  std::string url = "https://";
  url += kCoverArtArchiveHost;
  url += "/release-group/";
  url += mbid;
  url += "/front-500";
  return url;
}

std::string BuildCoverArtArchiveReleaseUrl(const std::string& mbid) {
  if (!IsUuid(mbid)) {
    return std::string();
  }
  std::string url = "https://";
  url += kCoverArtArchiveHost;
  url += "/release/";
  url += mbid;
  url += "/front-500";
  return url;
}

std::string BuildDiscogsSearchUrl(const std::string& artist, const std::string& album) {
  // Verified 2026-11-04: `/database/search?q=...&type=release&per_page=1` is
  // keyless today; search rows strip covers keyless, so the release id below
  // is followed with `GET /releases/<id>` for `images[0].uri`.
  if (artist.empty() && album.empty()) {
    return std::string();
  }
  const std::string terms = album.empty() ? artist : artist + " " + album;
  std::string url = "https://";
  url += kDiscogsHost;
  url += "/database/search?q=";
  url += UriEncodeUtf8(terms);
  url += "&type=release&per_page=1";
  return url;
}

std::string BuildDiscogsReleaseUrl(const std::string& release_id) {
  if (!IsAllDigits(release_id)) {
    return std::string();
  }
  std::string url = "https://";
  url += kDiscogsHost;
  url += "/releases/";
  url += release_id;
  return url;
}

std::string BuildWikipediaSearchUrl(const std::string& artist, const std::string& album) {
  // Verified 2026-11-04: pageimages is keyless but patchy (RAM has no free
  // pageimage); exact titles only, `pilicense=any`, thumbnails are fair-use
  // hotlinks, so this stays a fallback, never the primary cover source.
  if (artist.empty() && album.empty()) {
    return std::string();
  }
  const std::string terms = album.empty() ? artist : artist + " " + album;
  std::string url = "https://";
  url += kWikipediaHost;
  url += "/w/api.php?action=query&format=json&generator=search&gsrsearch=";
  url += UriEncodeUtf8(terms);
  url += "&gsrlimit=1&prop=pageimages&pithumbsize=600&pilicense=any";
  return url;
}

std::string ExtractMusicBrainzReleaseGroupId(const std::string& body) {
  const size_t anchor = body.find("\"release-groups\"");
  if (anchor == std::string::npos) {
    return std::string();
  }
  // Slicing before searching is the point: release-group objects nest
  // releases[].id and artist-credit[].artist.id values, so only the first "id"
  // after the "release-groups" key is the release-group MBID.
  return ExtractJsonStringField(body.substr(anchor), "id");
}

std::string ExtractMusicBrainzReleaseId(const std::string& body) {
  const size_t anchor = body.find("\"releases\"");
  if (anchor == std::string::npos) {
    return std::string();
  }
  return ExtractJsonStringField(body.substr(anchor), "id");
}

std::string ExtractDiscogsReleaseId(const std::string& body) {
  const size_t anchor = body.find("\"results\"");
  if (anchor == std::string::npos) {
    return std::string();
  }
  const std::string scoped = body.substr(anchor);
  const size_t key = scoped.find("\"id\"");
  if (key == std::string::npos) {
    return std::string();
  }
  size_t cursor = key + 4;
  while (cursor < scoped.size() &&
         std::isspace(static_cast<unsigned char>(scoped[cursor])) != 0) {
    ++cursor;
  }
  if (cursor >= scoped.size() || scoped[cursor] != ':') {
    return std::string();
  }
  ++cursor;
  while (cursor < scoped.size() &&
         std::isspace(static_cast<unsigned char>(scoped[cursor])) != 0) {
    ++cursor;
  }
  size_t end = cursor;
  while (end < scoped.size() && std::isdigit(static_cast<unsigned char>(scoped[end])) != 0) {
    ++end;
  }
  if (end == cursor) {
    return std::string();
  }
  return scoped.substr(cursor, end - cursor);
}

std::string ExtractDeezerTrackAlbumTitle(const std::string& body) {
  const size_t anchor = body.find("\"album\"");
  if (anchor == std::string::npos) {
    return std::string();
  }
  return ExtractJsonStringField(body.substr(anchor), "title");
}

std::string ExtractItunesAlbumTitle(const std::string& body) {
  return ExtractJsonStringField(body, "collectionName");
}

std::string ExtractWikipediaPageTitle(const std::string& body) {
  const size_t anchor = body.find("\"pages\"");
  if (anchor == std::string::npos) {
    return std::string();
  }
  return ExtractJsonStringField(body.substr(anchor), "title");
}

std::string ExtractWikipediaThumbnailUrl(const std::string& body) {
  const size_t anchor = body.find("\"thumbnail\"");
  if (anchor == std::string::npos) {
    return std::string();
  }
  const std::string url = ExtractJsonStringField(body.substr(anchor), "source");
  if (url.size() >= 8 && url.compare(0, 8, "https://") == 0 && url.size() <= kMaxUrlLength) {
    return url;
  }
  return std::string();
}

std::string ExtractDiscogsImageUrl(const std::string& body) {
  const size_t anchor = body.find("\"images\"");
  const std::string scoped = anchor == std::string::npos ? body : body.substr(anchor);
  return ExtractFirstUsableUrl(scoped, "uri");
}

// Word-prefix agreement: after normalization the shorter string's whole words
// must match the longer string's leading words. "Discovery" matches "Discovery
// (Deluxe Edition)" and "Live at Madison Square Garden" matches its "(Live)"
// variant, while "Program Music I" does not match "Program Music III" because
// the final words differ. Declared in the header; defined here, outside the
// anonymous namespace, because SearchResultMatches (inside it) calls it.
bool MetadataMatches(const std::string& expected, const std::string& candidate) {
  const std::string normalized_expected = NormalizeForMatch(expected);
  const std::string normalized_candidate = NormalizeForMatch(candidate);
  // A genuinely empty candidate is never a match. An empty *requested* value
  // means the caller is not comparing that field at all (an artist-only album
  // lookup), so it does not veto the comparison. A present expected value is
  // still compared strictly.
  if (normalized_candidate.empty()) {
    return false;
  }
  if (normalized_expected.empty()) {
    return true;
  }
  if (normalized_expected == normalized_candidate) {
    return true;
  }

  const std::vector<std::string> expected_words = SplitWords(normalized_expected);
  const std::vector<std::string> candidate_words = SplitWords(normalized_candidate);
  const std::vector<std::string>& short_words =
      expected_words.size() <= candidate_words.size() ? expected_words : candidate_words;
  const std::vector<std::string>& long_words =
      expected_words.size() <= candidate_words.size() ? candidate_words : expected_words;

  for (size_t index = 0; index < short_words.size(); ++index) {
    if (short_words[index] != long_words[index]) {
      return false;
    }
  }
  return true;
}

bool SearchResultMatches(const std::string& body, Provider provider,
                         const std::string& artist, const std::string& album) {
  if (provider == Provider::kWikipedia) {
    // Exact-title guard (verified 2026-11-04: "Mezzanine (album)" resolves,
    // "Mezzanine (Massive Attack album)" is missing, so the page title must
    // agree with the requested album, not just rank first).
    const std::string hit_title = ExtractWikipediaPageTitle(body);
    if (hit_title.empty()) {
      return false;
    }
    const std::string expected = album.empty() ? artist : album;
    return MetadataMatches(expected, hit_title);
  }

  if (provider == Provider::kDiscogs) {
    // Search rows name releases as "Artist - Album" (verified 2026-11-04:
    // "Pink Floyd - The Dark Side Of The Moon").
    const size_t anchor = body.find("\"results\"");
    if (anchor == std::string::npos) {
      return false;
    }
    const std::string hit_title =
        ExtractJsonStringField(body.substr(anchor), "title");
    if (hit_title.empty()) {
      return false;
    }
    const std::string expected = album.empty() ? artist : artist + " " + album;
    return MetadataMatches(expected, hit_title);
  }

  std::string hit_artist;
  std::string hit_album;
  if (!ExtractSearchHitIdentity(body, provider, &hit_artist, &hit_album)) {
    return false;
  }
  // An empty album is an artist-only request: the guard compares the artist and
  // deliberately does not compare the album (see MetadataMatches), so a hit for
  // a real album by that artist is adopted. A present album is compared
  // strictly, so a hit naming a different album is still rejected.
  return MetadataMatches(artist, hit_artist) && MetadataMatches(album, hit_album);
}

bool DeezerTrackResultMatches(const std::string& body, const std::string& artist,
                              const std::string& track) {
  if (artist.empty() || track.empty() || body.empty()) {
    return false;
  }
  const std::string hit_track = ExtractJsonStringField(body, "title");
  const size_t artist_key = body.find("\"artist\"");
  const std::string hit_artist = artist_key == std::string::npos
                                     ? std::string()
                                     : ExtractJsonStringField(body.substr(artist_key), "name");
  const std::string hit_album = ExtractDeezerTrackAlbumTitle(body);
  if (hit_artist.empty() || hit_track.empty() || hit_album.empty()) {
    return false;
  }
  return MetadataMatches(artist, hit_artist) && MetadataMatches(track, hit_track);
}

bool ItunesSongResultMatches(const std::string& body, const std::string& artist,
                             const std::string& track) {
  if (artist.empty() || track.empty() || body.empty()) {
    return false;
  }
  const std::string hit_artist = ExtractJsonStringField(body, "artistName");
  const std::string hit_track = ExtractJsonStringField(body, "trackName");
  const std::string hit_album = ExtractJsonStringField(body, "collectionName");
  if (hit_artist.empty() || hit_track.empty() || hit_album.empty()) {
    return false;
  }
  return MetadataMatches(artist, hit_artist) && MetadataMatches(track, hit_track);
}

std::string ExtractArtworkUrl(const std::string& body, Provider provider) {
  if (body.empty()) {
    return std::string();
  }

  if (provider == Provider::kDeezer) {
    for (const char* key : {"cover_xl", "cover_big", "cover"}) {
      const std::string url = ExtractFirstUsableUrl(body, key);
      if (!url.empty()) {
        return url;
      }
    }
    return std::string();
  }

  if (provider == Provider::kItunes) {
    for (const char* key : {"artworkUrl100", "artworkUrl60", "artworkUrl30"}) {
      const std::string url = ExtractFirstUsableUrl(body, key);
      if (!url.empty()) {
        return UpscaleItunesArtwork(url);
      }
    }
    return std::string();
  }

  if (provider == Provider::kWikipedia) {
    return ExtractWikipediaThumbnailUrl(body);
  }

  if (provider == Provider::kDiscogs) {
    return ExtractDiscogsImageUrl(body);
  }

  return std::string();
}

Resolver::Resolver() { worker_ = std::thread(&Resolver::WorkerMain, this); }

Resolver::~Resolver() { Shutdown(); }

void Resolver::SetOnlineEnabled(bool enabled) {
  {
    std::lock_guard<std::mutex> lock(mutex_);
    online_ = enabled;
  }
  wake_.notify_all();
}

void Resolver::SetCallback(Callback callback) {
  std::lock_guard<std::mutex> lock(mutex_);
  callback_ = std::move(callback);
}

void Resolver::SetLogger(Logger logger) {
  std::lock_guard<std::mutex> lock(mutex_);
  logger_ = std::move(logger);
}

void Resolver::Request(const std::string& artist, const std::string& album,
                       const std::string& file_path, const std::string& track) {
  {
    std::lock_guard<std::mutex> lock(mutex_);
    if (stopping_) {
      return;
    }
    request_artist_ = artist;
    request_album_ = album;
    request_file_path_ = file_path;
    request_track_ = track;
    has_request_ = true;
    ++generation_;
  }
  wake_.notify_all();
}

std::string Resolver::Lookup(const std::string& artist, const std::string& album,
                             const std::string& file_path, const std::string& track) {
  const std::string key = BuildAlbumKey(artist, album, file_path);

  Log("online-art requested artist=\"" + artist + "\" album=\"" + album + "\"");

  std::string cached;
  if (TakeCached(key, &cached)) {
    // Only definitive answers are cached, so a cached empty URL is a real miss.
    LogOutcome(artist, album, cached);
    return cached;
  }

  bool definitive = false;
  const std::string url = LookupUncached(artist, album, track, &definitive);
  if (definitive) {
    StoreCached(key, url);
    // A transport failure is not definitive and deliberately logs no outcome:
    // the same query is retried on the next request.
    LogOutcome(artist, album, url);
  }
  return url;
}

std::string Resolver::ResolveAlbumTitle(const std::string& artist,
                                        const std::string& track) {
  if (artist.empty() || track.empty() || stopping_.load()) {
    return std::string();
  }

  // Deezer strict track search first: it returns the album object inline.
  {
    const std::string url = BuildDeezerTrackSearchUrl(artist, track);
    if (!url.empty() && !stopping_.load()) {
      std::string body;
      if (HttpGet(url, &body) && DeezerTrackResultMatches(body, artist, track)) {
        const std::string album = ExtractDeezerTrackAlbumTitle(body);
        if (!album.empty()) {
          return album;
        }
      }
    }
  }

  if (stopping_.load()) {
    return std::string();
  }

  // iTunes song search: the hit's collectionName is the album.
  {
    const std::string url = BuildItunesSongSearchUrl(artist, track);
    if (!url.empty() && !stopping_.load()) {
      std::string body;
      if (HttpGet(url, &body) && ItunesSongResultMatches(body, artist, track)) {
        const std::string album = ExtractItunesAlbumTitle(body);
        if (!album.empty()) {
          return album;
        }
      }
    }
  }

  return std::string();
}

std::string Resolver::LookupFromProvider(Provider provider, const std::string& artist,
                                         const std::string& album, bool* answered) {
  if (answered != nullptr) {
    *answered = false;
  }
  if (stopping_.load()) {
    return std::string();
  }

  if (provider == Provider::kDeezer) {
    const std::string url = BuildDeezerSearchUrl(artist, album);
    if (url.empty()) {
      if (answered != nullptr) {
        *answered = true;
      }
      return std::string();
    }
    std::string body;
    if (!HttpGet(url, &body)) {
      return std::string();
    }
    if (answered != nullptr) {
      *answered = true;
    }
    if (!SearchResultMatches(body, Provider::kDeezer, artist, album)) {
      return std::string();
    }
    return ExtractArtworkUrl(body, Provider::kDeezer);
  }

  if (provider == Provider::kItunes) {
    const std::string url = BuildItunesSearchUrl(artist, album);
    if (url.empty()) {
      if (answered != nullptr) {
        *answered = true;
      }
      return std::string();
    }
    std::string body;
    if (!HttpGet(url, &body)) {
      return std::string();
    }
    if (answered != nullptr) {
      *answered = true;
    }
    if (!SearchResultMatches(body, Provider::kItunes, artist, album)) {
      return std::string();
    }
    return ExtractArtworkUrl(body, Provider::kItunes);
  }

  if (provider == Provider::kWikipedia) {
    const std::string url = BuildWikipediaSearchUrl(artist, album);
    if (url.empty()) {
      if (answered != nullptr) {
        *answered = true;
      }
      return std::string();
    }
    std::string body;
    if (!HttpGet(url, &body)) {
      return std::string();
    }
    if (answered != nullptr) {
      *answered = true;
    }
    if (!SearchResultMatches(body, Provider::kWikipedia, artist, album)) {
      return std::string();
    }
    return ExtractArtworkUrl(body, Provider::kWikipedia);
  }

  if (provider == Provider::kDiscogs) {
    const std::string url = BuildDiscogsSearchUrl(artist, album);
    if (url.empty()) {
      if (answered != nullptr) {
        *answered = true;
      }
      return std::string();
    }
    WaitForDiscogsSlot();
    if (stopping_.load()) {
      return std::string();
    }
    std::string body;
    if (!HttpGet(url, &body, kUserAgent)) {
      return std::string();
    }
    if (!SearchResultMatches(body, Provider::kDiscogs, artist, album)) {
      if (answered != nullptr) {
        *answered = true;
      }
      return std::string();
    }
    const std::string release_id = ExtractDiscogsReleaseId(body);
    if (release_id.empty()) {
      if (answered != nullptr) {
        *answered = true;
      }
      return std::string();
    }
    const std::string detail_url = BuildDiscogsReleaseUrl(release_id);
    if (detail_url.empty()) {
      if (answered != nullptr) {
        *answered = true;
      }
      return std::string();
    }
    if (stopping_.load()) {
      return std::string();
    }
    WaitForDiscogsSlot();
    if (stopping_.load()) {
      return std::string();
    }
    std::string detail;
    int status = 0;
    if (!HttpGet(detail_url, &detail, kUserAgent, &status)) {
      if (status == 404 && answered != nullptr) {
        // The search hit is gone: a definitive miss, not a retry.
        *answered = true;
      }
      return std::string();
    }
    if (answered != nullptr) {
      *answered = true;
    }
    return ExtractArtworkUrl(detail, Provider::kDiscogs);
  }

  if (provider == Provider::kMusicBrainzRelease) {
    const std::string url = BuildMusicBrainzReleaseSearchUrl(artist, album);
    if (url.empty()) {
      if (answered != nullptr) {
        *answered = true;
      }
      return std::string();
    }
    WaitForMusicBrainzSlot();
    if (stopping_.load()) {
      return std::string();
    }
    std::string body;
    if (!HttpGet(url, &body, kUserAgent)) {
      return std::string();
    }
    if (!SearchResultMatches(body, Provider::kMusicBrainzRelease, artist, album)) {
      if (answered != nullptr) {
        *answered = true;
      }
      return std::string();
    }
    const std::string mbid = ExtractMusicBrainzReleaseId(body);
    const std::string cover_url = BuildCoverArtArchiveReleaseUrl(mbid);
    if (cover_url.empty()) {
      if (answered != nullptr) {
        *answered = true;
      }
      return std::string();
    }
    if (stopping_.load()) {
      return std::string();
    }
    std::string cover_body;
    int cover_status = 0;
    if (HttpGet(cover_url, &cover_body, kUserAgent, &cover_status)) {
      if (answered != nullptr) {
        *answered = true;
      }
      return cover_url;
    }
    if (cover_status == 404 && answered != nullptr) {
      // A 404 is a normal Cover Art Archive miss (coverage is not complete).
      *answered = true;
    }
    return std::string();
  }

  if (provider == Provider::kMusicBrainz) {
    // Combined: release-group first, then the release-search fallback. Both
    // legs are gated separately so consecutive resolves measure from the
    // previous request's start.
    bool group_answered = false;
    bool release_answered = false;

    {
      const std::string url = BuildMusicBrainzReleaseGroupUrl(artist, album);
      if (url.empty()) {
        group_answered = true;
      } else if (!stopping_.load()) {
        WaitForMusicBrainzSlot();
        if (!stopping_.load()) {
          std::string body;
          if (HttpGet(url, &body, kUserAgent)) {
            group_answered = true;
            if (SearchResultMatches(body, Provider::kMusicBrainz, artist, album)) {
              const std::string mbid = ExtractMusicBrainzReleaseGroupId(body);
              const std::string cover_url = BuildCoverArtArchiveUrl(mbid);
              if (!cover_url.empty() && !stopping_.load()) {
                std::string cover_body;
                int cover_status = 0;
                if (HttpGet(cover_url, &cover_body, kUserAgent, &cover_status)) {
                  if (answered != nullptr) {
                    *answered = true;
                  }
                  return cover_url;
                }
                if (cover_status != 404) {
                  group_answered = false;
                }
              }
            }
          }
        }
      }
      if (stopping_.load()) {
        return std::string();
      }
      // A release-group cover hit already returned above; a group miss falls
      // through to the release fallback below.
    }

    {
      const std::string url = BuildMusicBrainzReleaseSearchUrl(artist, album);
      if (url.empty()) {
        release_answered = true;
      } else if (!stopping_.load()) {
        WaitForMusicBrainzSlot();
        if (!stopping_.load()) {
          std::string body;
          if (HttpGet(url, &body, kUserAgent)) {
            release_answered = true;
            if (SearchResultMatches(body, Provider::kMusicBrainzRelease, artist, album)) {
              const std::string mbid = ExtractMusicBrainzReleaseId(body);
              const std::string cover_url = BuildCoverArtArchiveReleaseUrl(mbid);
              if (!cover_url.empty() && !stopping_.load()) {
                std::string cover_body;
                int cover_status = 0;
                if (HttpGet(cover_url, &cover_body, kUserAgent, &cover_status)) {
                  if (answered != nullptr) {
                    *answered = true;
                  }
                  return cover_url;
                }
                if (cover_status != 404) {
                  release_answered = false;
                }
              }
            }
          }
        }
      }
      if (stopping_.load()) {
        return std::string();
      }
    }

    if (answered != nullptr) {
      *answered = group_answered && release_answered;
    }
    return std::string();
  }

  if (answered != nullptr) {
    *answered = true;
  }
  return std::string();
}

void Resolver::Log(const std::string& line) {
  Logger logger;
  {
    std::lock_guard<std::mutex> lock(mutex_);
    logger = logger_;
  }
  if (logger) {
    logger(line);
  }
}

void Resolver::LogOutcome(const std::string& artist, const std::string& album,
                          const std::string& url) {
  if (url.empty()) {
    Log("online-art not-found artist=\"" + artist + "\" album=\"" + album + "\"");
    return;
  }
  Log("online-art resolved artist=\"" + artist + "\" album=\"" + album + "\" url=\"" +
      url + "\"");
}

bool Resolver::TakeCached(const std::string& key, std::string* url) {
  std::lock_guard<std::mutex> lock(mutex_);
  const auto found = cache_.find(key);
  if (found == cache_.end()) {
    return false;
  }
  *url = found->second;
  return true;
}

void Resolver::StoreCached(const std::string& key, const std::string& url) {
  std::lock_guard<std::mutex> lock(mutex_);
  const auto inserted = cache_.insert({key, url});
  if (!inserted.second) {
    return;
  }
  cache_order_.push_back(key);

  while (cache_order_.size() > kMaxCacheEntries) {
    const std::string oldest = cache_order_.front();
    cache_order_.pop_front();
    cache_.erase(oldest);
  }
}

std::string Resolver::LookupUncached(const std::string& artist, const std::string& album,
                                     const std::string& track, bool* definitive) {
  *definitive = false;

  if (artist.empty()) {
    // There is genuinely nothing to query and no point in retrying later. An
    // empty album is not nothing: it is resolved from the track title below,
    // and when resolution fails the providers are still searched by artist
    // alone with the identity guard accepting an artist-only hit (see
    // SearchResultMatches), so an album-less track still reaches the online
    // chain instead of being refused here and falling through to the black PNG.
    *definitive = true;
    return std::string();
  }

  if (stopping_.load()) {
    return std::string();
  }

  // Empty-album resolution: the track title names the album, guarded by artist
  // + title, and the resolved album becomes the effective album for the whole
  // art lookup below.
  std::string effective_album = album;
  if (effective_album.empty() && !track.empty()) {
    const std::string resolved = ResolveAlbumTitle(artist, track);
    if (!resolved.empty()) {
      effective_album = resolved;
    }
    if (stopping_.load()) {
      return std::string();
    }
  }

  // Abort between tiers when AIMP is unloading; failure to answer is never a
  // definitive negative result. A provider that answered but whose hit did not
  // survive the identity guard has still given a definitive answer: retrying
  // the same query would not change it.
  using ProviderAnswer = std::pair<std::string, bool>;
  const auto launch_provider = [this](Provider provider, const std::string& lookup_artist,
                                      const std::string& lookup_album) {
    return std::async(std::launch::async, [this, provider, lookup_artist, lookup_album]() {
      bool answered = false;
      std::string url;
      if (!stopping_.load()) {
        url = LookupFromProvider(provider, lookup_artist, lookup_album, &answered);
      }
      return ProviderAnswer(url, answered);
    });
  };

  // Tier 1: Deezer + iTunes in parallel. The common case ends here, so the
  // enrichment hosts below are never woken for a track Tier 1 already covers.
  std::future<ProviderAnswer> future_deezer =
      launch_provider(Provider::kDeezer, artist, effective_album);
  std::future<ProviderAnswer> future_itunes =
      launch_provider(Provider::kItunes, artist, effective_album);
  const ProviderAnswer deezer = future_deezer.get();
  if (stopping_.load()) {
    // Drain the sibling before returning so no async thread outlives the
    // resolve that spawned it.
    (void)future_itunes.get();
    return std::string();
  }
  const ProviderAnswer itunes = future_itunes.get();
  if (stopping_.load()) {
    return std::string();
  }
  if (!deezer.first.empty()) {
    *definitive = true;
    return deezer.first;
  }
  if (!itunes.first.empty()) {
    *definitive = true;
    return itunes.first;
  }
  const bool tier1_answered = deezer.second && itunes.second;
  if (!tier1_answered) {
    // A Tier 1 transport failure must not hide the album art until cache
    // eviction, yet Tier 2 may still have a cover: run it, and only cache the
    // miss when every reached provider actually answered.
    ;
  }

  if (stopping_.load()) {
    return std::string();
  }

  // Tier 2: MusicBrainz (release-group then release fallback) + Wikipedia +
  // Discogs in parallel. Runs only when Tier 1 missed both.
  std::future<ProviderAnswer> future_mb =
      launch_provider(Provider::kMusicBrainz, artist, effective_album);
  std::future<ProviderAnswer> future_wiki =
      launch_provider(Provider::kWikipedia, artist, effective_album);
  std::future<ProviderAnswer> future_discogs =
      launch_provider(Provider::kDiscogs, artist, effective_album);
  const ProviderAnswer mb = future_mb.get();
  if (stopping_.load()) {
    (void)future_wiki.get();
    (void)future_discogs.get();
    return std::string();
  }
  const ProviderAnswer wiki = future_wiki.get();
  if (stopping_.load()) {
    (void)future_discogs.get();
    return std::string();
  }
  const ProviderAnswer discogs = future_discogs.get();
  if (stopping_.load()) {
    return std::string();
  }

  if (!mb.first.empty()) {
    *definitive = true;
    return mb.first;
  }
  if (!wiki.first.empty()) {
    *definitive = true;
    return wiki.first;
  }
  if (!discogs.first.empty()) {
    *definitive = true;
    return discogs.first;
  }

  // Only a negative result that every reached provider actually answered may be
  // cached: a timeout or DNS failure must not hide the album art until cache
  // eviction.
  *definitive = tier1_answered && mb.second && wiki.second && discogs.second;
  return std::string();
}

void Resolver::WaitForMusicBrainzSlot() {
  const auto interval = std::chrono::milliseconds(kMusicBrainzIntervalMilliseconds);
  for (;;) {
    if (stopping_.load()) {
      return;
    }
    {
      std::lock_guard<std::mutex> lock(gate_mutex_);
      const auto now = std::chrono::steady_clock::now();
      const auto elapsed =
          std::chrono::duration_cast<std::chrono::milliseconds>(now - last_musicbrainz_request_);
      if (elapsed >= interval) {
        // Claim the slot before the request so consecutive resolves measure the
        // interval from the previous request's start.
        last_musicbrainz_request_ = now;
        return;
      }
    }
    // Sleep without the lock so Tier 2 siblings proceed while this leg waits.
    // Short slices keep an unload prompt.
    std::this_thread::sleep_for(std::chrono::milliseconds(50));
  }
}

void Resolver::WaitForDiscogsSlot() {
  const auto interval = std::chrono::milliseconds(kDiscogsIntervalMilliseconds);
  for (;;) {
    if (stopping_.load()) {
      return;
    }
    {
      std::lock_guard<std::mutex> lock(gate_mutex_);
      const auto now = std::chrono::steady_clock::now();
      const auto elapsed =
          std::chrono::duration_cast<std::chrono::milliseconds>(now - last_discogs_request_);
      if (elapsed >= interval) {
        last_discogs_request_ = now;
        return;
      }
    }
    std::this_thread::sleep_for(std::chrono::milliseconds(50));
  }
}

void Resolver::WorkerMain() {
  for (;;) {
    std::unique_lock<std::mutex> lock(mutex_);
    wake_.wait(lock, [this] { return stopping_ || has_request_; });
    if (stopping_) {
      break;
    }

    has_request_ = false;
    const uint64_t generation = generation_;
    const bool online = online_;
    const std::string artist = request_artist_;
    const std::string album = request_album_;
    const std::string file_path = request_file_path_;
    const std::string track = request_track_;
    const Callback callback = callback_;
    lock.unlock();

    std::string url;
    if (online) {
      url = Lookup(artist, album, file_path, track);
    }

    lock.lock();
    const bool stale = stopping_ || generation != generation_;
    lock.unlock();

    if (!stale && callback) {
      callback(artist, album, file_path, url);
    }
  }
}

void Resolver::Shutdown() {
  {
    std::lock_guard<std::mutex> lock(mutex_);
    if (stopping_) {
      return;
    }
    stopping_ = true;
  }
  wake_.notify_all();

  if (worker_.joinable()) {
    worker_.join();
  }
}

}  // namespace AlbumArt
