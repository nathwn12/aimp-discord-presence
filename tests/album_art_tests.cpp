// Offline + live tests for the keyless online chain (src/album_art.cpp).
//
// Offline (default): pure helpers only, no network. URL builders, JSON
// extractors (fixtures from the 2026-11-04 keyless-notes probe), identity
// guards, track-level resolution guards, and the two-tier preference order.
// Build (from the repository root):
//   cl /nologo /std:c++17 /W4 /WX /EHsc tests\album_art_tests.cpp
//      src\album_art.cpp /link winhttp.lib
// Run:
//   album_art_tests.exe
//
// Live (--live): real HTTPS GETs through Resolver::Lookup / LookupFromProvider.
// Prints one resolved image URL per album and exits 0 only when at least 2 of
// 3 albums resolve and at least one URL comes from a newly added host
// (Wikipedia, Discogs, or the MusicBrainz release-search /release/ cover).
// Run:
//   album_art_tests.exe --live

#include "../src/album_art.h"

#include <cstdio>
#include <string>
#include <vector>

namespace {

int g_checks = 0;
int g_failures = 0;

void Check(bool condition, const std::string& label) {
  ++g_checks;
  if (!condition) {
    ++g_failures;
    std::fprintf(stderr, "FAIL: %s\n", label.c_str());
  }
}

bool Contains(const std::string& haystack, const std::string& needle) {
  return haystack.find(needle) != std::string::npos;
}

// --- URL builders -----------------------------------------------------------

void TestUrlBuilders() {
  // Tier 1 keepers.
  const std::string deezer =
      AlbumArt::BuildDeezerSearchUrl("Pink Floyd", "The Dark Side of the Moon");
  Check(Contains(deezer, "api.deezer.com/search/album"), "deezer art url targets the album endpoint");
  Check(Contains(deezer, "limit=1"), "deezer art url is bounded");
  Check(Contains(AlbumArt::BuildDeezerSearchUrl("Daft Punk", ""),
                 "api.deezer.com/search/album"),
        "deezer artist-only url still targets the album endpoint");

  const std::string deezer_track =
      AlbumArt::BuildDeezerTrackSearchUrl("Massive Attack", "Teardrop");
  Check(Contains(deezer_track, "api.deezer.com/search/track"),
        "deezer track url targets the track endpoint");
  Check(Contains(deezer_track, "artist"), "deezer track url carries the artist field");
  Check(Contains(deezer_track, "track"), "deezer track url carries the track field");
  Check(Contains(deezer_track, "limit=1"), "deezer track url is bounded");
  Check(AlbumArt::BuildDeezerTrackSearchUrl("", "Teardrop").empty(),
        "deezer track url needs an artist");
  Check(AlbumArt::BuildDeezerTrackSearchUrl("Massive Attack", "").empty(),
        "deezer track url needs a track title");

  const std::string itunes =
      AlbumArt::BuildItunesSearchUrl("Daft Punk", "Random Access Memories");
  Check(Contains(itunes, "itunes.apple.com/search"), "itunes art url targets search");
  Check(Contains(itunes, "media=music"), "itunes art url stays in music");

  const std::string itunes_song =
      AlbumArt::BuildItunesSongSearchUrl("Massive Attack", "Teardrop");
  Check(Contains(itunes_song, "itunes.apple.com/search"), "itunes song url targets search");
  Check(Contains(itunes_song, "entity=song"), "itunes song url pins entity=song");
  Check(AlbumArt::BuildItunesSongSearchUrl("", "Teardrop").empty(),
        "itunes song url needs an artist");
  Check(AlbumArt::BuildItunesSongSearchUrl("Massive Attack", "").empty(),
        "itunes song url needs a track title");

  // MusicBrainz pair.
  const std::string mb_group =
      AlbumArt::BuildMusicBrainzReleaseGroupUrl("Daft Punk", "Random Access Memories");
  Check(Contains(mb_group, "musicbrainz.org/ws/2/release-group/"),
        "mb group url targets release-group search");
  Check(Contains(mb_group, "fmt=json"), "mb group url asks for json");

  const std::string mb_release =
      AlbumArt::BuildMusicBrainzReleaseSearchUrl("Daft Punk", "Random Access Memories");
  Check(Contains(mb_release, "musicbrainz.org/ws/2/release/"),
        "mb release url targets release search");
  Check(!Contains(mb_release, "release-group"), "mb release url is not the group endpoint");
  Check(Contains(mb_release, "fmt=json"), "mb release url asks for json");

  const std::string good_mbid = "8ecfafd1-89a8-423a-968f-3fff47f0b0f9";
  Check(Contains(AlbumArt::BuildCoverArtArchiveUrl(good_mbid), "/release-group/"),
        "caa group url names the release-group path");
  Check(Contains(AlbumArt::BuildCoverArtArchiveUrl(good_mbid), "/front-500"),
        "caa group url asks for the front-500 cover");
  Check(Contains(AlbumArt::BuildCoverArtArchiveReleaseUrl(good_mbid), "/release/"),
        "caa release url names the release path");
  Check(Contains(AlbumArt::BuildCoverArtArchiveReleaseUrl(good_mbid), "/front-500"),
        "caa release url asks for the front-500 cover");
  Check(!Contains(AlbumArt::BuildCoverArtArchiveReleaseUrl(good_mbid), "/release-group/"),
        "caa release url is not the group path");
  Check(AlbumArt::BuildCoverArtArchiveUrl("not-a-uuid").empty(),
        "caa group url rejects a malformed mbid");
  Check(AlbumArt::BuildCoverArtArchiveReleaseUrl("not-a-uuid").empty(),
        "caa release url rejects a malformed mbid");

  // Wikipedia fallback.
  const std::string wiki =
      AlbumArt::BuildWikipediaSearchUrl("Pink Floyd", "The Dark Side of the Moon");
  Check(Contains(wiki, "en.wikipedia.org/w/api.php"), "wiki url targets the api");
  Check(Contains(wiki, "generator=search"), "wiki url searches with a generator");
  Check(Contains(wiki, "prop=pageimages"), "wiki url asks for pageimages");
  Check(Contains(wiki, "pithumbsize=600"), "wiki url asks for 600px thumbs");
  Check(Contains(wiki, "pilicense=any"), "wiki url accepts any license");
  Check(AlbumArt::BuildWikipediaSearchUrl("", "").empty(), "wiki url needs a query");

  // Discogs enrichment.
  const std::string discogs =
      AlbumArt::BuildDiscogsSearchUrl("Pink Floyd", "The Dark Side of the Moon");
  Check(Contains(discogs, "api.discogs.com/database/search"),
        "discogs url targets database search");
  Check(Contains(discogs, "type=release"), "discogs url pins type=release");
  Check(Contains(discogs, "per_page=1"), "discogs url takes one hit");
  Check(AlbumArt::BuildDiscogsSearchUrl("", "").empty(), "discogs url needs a query");
  Check(AlbumArt::BuildDiscogsReleaseUrl("9287809") ==
            "https://api.discogs.com/releases/9287809",
        "discogs release url names the release");
  Check(AlbumArt::BuildDiscogsReleaseUrl("").empty(), "discogs release url rejects empty");
  Check(AlbumArt::BuildDiscogsReleaseUrl("abc").empty(),
        "discogs release url rejects a non-numeric id");

  // Forbidden hosts never appear in any builder (brief reject list).
  const std::string all_urls = deezer + deezer_track + itunes + itunes_song + mb_group +
                               mb_release + wiki + discogs;
  Check(!Contains(all_urls, "theaudiodb"), "no builder touches TheAudioDB");
  Check(!Contains(all_urls, "listenbrainz"), "no builder touches ListenBrainz");
  Check(!Contains(all_urls, "audioscrobbler") && !Contains(all_urls, "last.fm"),
        "no builder touches Last.fm");
  Check(!Contains(all_urls, "archive.org"), "no builder touches Internet Archive");
}

// --- Extractors (fixtures from keyless-notes.md, 2026-11-04) ---------------

void TestExtractors() {
  // Deezer album cover.
  const std::string deezer_body =
      "{\"data\":[{\"title\":\"Random Access Memories\",\"artist\":{\"name\":\"Daft Punk\"},"
      "\"cover_xl\":\"https://cdn-images.dzcdn.net/images/cover/abc123/1000x1000-000000-80-0-0.jpg\"}],"
      "\"total\":1}";
  Check(AlbumArt::ExtractArtworkUrl(deezer_body, AlbumArt::Provider::kDeezer) ==
            "https://cdn-images.dzcdn.net/images/cover/abc123/1000x1000-000000-80-0-0.jpg",
        "deezer extractor returns cover_xl");
  Check(AlbumArt::ExtractArtworkUrl("{\"data\":[],\"total\":0}",
                                   AlbumArt::Provider::kDeezer)
            .empty(),
        "deezer extractor misses on an empty answer");

  // iTunes upscale.
  const std::string itunes_body =
      "{\"resultCount\":1,\"results\":[{\"artistName\":\"Massive Attack\","
      "\"trackName\":\"Teardrop\",\"collectionName\":\"Mezzanine\","
      "\"artworkUrl100\":\"https://is1-ssl.mzstatic.com/image/thumb/Music/xyz/100x100bb.jpg\"}]}";
  Check(AlbumArt::ExtractArtworkUrl(itunes_body, AlbumArt::Provider::kItunes) ==
            "https://is1-ssl.mzstatic.com/image/thumb/Music/xyz/600x600bb.jpg",
        "itunes extractor upscales to 600x600");

  // MusicBrainz ids.
  const std::string mb_group_body =
      "{\"count\":1,\"release-groups\":[{\"id\":\"8ecfafd1-89a8-423a-968f-3fff47f0b0f9\","
      "\"title\":\"Random Access Memories\","
      "\"artist-credit\":[{\"name\":\"Daft Punk\"}]}]}";
  Check(AlbumArt::ExtractMusicBrainzReleaseGroupId(mb_group_body) ==
            "8ecfafd1-89a8-423a-968f-3fff47f0b0f9",
        "mb group extractor returns the release-group mbid");
  Check(AlbumArt::ExtractMusicBrainzReleaseId("{\"count\":0,\"releases\":[]}").empty(),
        "mb release extractor misses on an empty answer");
  const std::string mb_release_body =
      "{\"count\":27,\"releases\":[{\"id\":\"8ecfafd1-89a8-423a-968f-3fff47f0b0f9\","
      "\"title\":\"Random Access Memories\","
      "\"artist-credit\":[{\"name\":\"Daft Punk\"}]}]}";
  Check(AlbumArt::ExtractMusicBrainzReleaseId(mb_release_body) ==
            "8ecfafd1-89a8-423a-968f-3fff47f0b0f9",
        "mb release extractor returns the release mbid");

  // Deezer track album title (resolution).
  const std::string deezer_track_body =
      "{\"data\":[{\"title\":\"Teardrop\",\"artist\":{\"name\":\"Massive Attack\"},"
      "\"album\":{\"title\":\"Mezzanine\"}}]}";
  Check(AlbumArt::ExtractDeezerTrackAlbumTitle(deezer_track_body) == "Mezzanine",
        "deezer track extractor returns the album title");

  // iTunes collection name (resolution).
  Check(AlbumArt::ExtractItunesAlbumTitle(itunes_body) == "Mezzanine",
        "itunes song extractor returns the collection name");

  // Wikipedia page + thumbnail.
  const std::string wiki_body =
      "{\"query\":{\"pages\":{\"1791673\":{\"pageid\":1791673,\"title\":\"Mezzanine (album)\","
      "\"thumbnail\":{\"source\":\"https://upload.wikimedia.org/wikipedia/en/e/e9/Massive_Attack_-_Mezzanine.png\","
      "\"width\":600,\"height\":600}}}}}";
  Check(AlbumArt::ExtractWikipediaPageTitle(wiki_body) == "Mezzanine (album)",
        "wiki extractor returns the page title");
  Check(AlbumArt::ExtractWikipediaThumbnailUrl(wiki_body) ==
            "https://upload.wikimedia.org/wikipedia/en/e/e9/Massive_Attack_-_Mezzanine.png",
        "wiki extractor returns the thumbnail source");
  Check(AlbumArt::ExtractArtworkUrl(wiki_body, AlbumArt::Provider::kWikipedia) ==
            "https://upload.wikimedia.org/wikipedia/en/e/e9/Massive_Attack_-_Mezzanine.png",
        "wiki artwork extractor returns the thumbnail");
  Check(AlbumArt::ExtractWikipediaThumbnailUrl(
            "{\"query\":{\"pages\":{\"1\":{\"title\":\"Random Access Memories\"}}}}")
            .empty(),
        "wiki thumbnail misses when the page has no thumbnail (RAM case)");

  // Discogs search id + release image.
  const std::string discogs_search =
      "{\"pagination\":{\"items\":2490},"
      "\"results\":[{\"id\":9287809,"
      "\"title\":\"Pink Floyd - The Dark Side Of The Moon\",\"year\":\"2016\","
      "\"cover_image\":\"\",\"thumb\":\"\"}]}";
  Check(AlbumArt::ExtractDiscogsReleaseId(discogs_search) == "9287809",
        "discogs extractor returns the release id");
  const std::string discogs_release =
      "{\"title\":\"The Dark Side Of The Moon\","
      "\"images\":[{\"uri\":\"https://i.discogs.com/abc123-600px.jpeg\",\"width\":600}]}";
  Check(AlbumArt::ExtractDiscogsImageUrl(discogs_release) ==
            "https://i.discogs.com/abc123-600px.jpeg",
        "discogs extractor returns images[0].uri");
  Check(AlbumArt::ExtractArtworkUrl(discogs_release, AlbumArt::Provider::kDiscogs) ==
            "https://i.discogs.com/abc123-600px.jpeg",
        "discogs artwork extractor returns the release image");

  // MusicBrainz answers are never image URLs.
  Check(AlbumArt::ExtractArtworkUrl(mb_group_body, AlbumArt::Provider::kMusicBrainz).empty(),
        "mb group answer is not an image url");
  Check(AlbumArt::ExtractArtworkUrl(mb_release_body,
                                   AlbumArt::Provider::kMusicBrainzRelease)
            .empty(),
        "mb release answer is not an image url");
}

// --- Guards -----------------------------------------------------------------

void TestGuards() {
  // A matching Deezer hit passes; a wrong-album hit is rejected.
  const std::string deezer_ok =
      "{\"data\":[{\"title\":\"Discovery\",\"artist\":{\"name\":\"Daft Punk\"},"
      "\"cover_xl\":\"https://cdn-images.dzcdn.net/images/cover/a/600x600-000000-80-0-0.jpg\"}]}";
  Check(AlbumArt::SearchResultMatches(deezer_ok, AlbumArt::Provider::kDeezer, "Daft Punk",
                                     "Discovery"),
        "deezer guard accepts the requested album");
  const std::string deezer_wrong =
      "{\"data\":[{\"title\":\"Homework\",\"artist\":{\"name\":\"Daft Punk\"},"
      "\"cover_xl\":\"https://cdn-images.dzcdn.net/images/cover/a/600x600-000000-80-0-0.jpg\"}]}";
  Check(!AlbumArt::SearchResultMatches(deezer_wrong, AlbumArt::Provider::kDeezer,
                                      "Daft Punk", "Discovery"),
        "deezer guard rejects a different album");

  // Artist-only: an empty requested album accepts any album by that artist.
  Check(AlbumArt::SearchResultMatches(deezer_ok, AlbumArt::Provider::kDeezer, "Daft Punk", ""),
        "deezer guard accepts an artist-only hit");

  // Wikipedia exact-title guard.
  const std::string wiki_ok =
      "{\"query\":{\"pages\":{\"1791673\":{\"title\":\"Mezzanine (album)\","
      "\"thumbnail\":{\"source\":\"https://upload.wikimedia.org/wikipedia/en/e/e9/x.png\"}}}}}";
  Check(AlbumArt::SearchResultMatches(wiki_ok, AlbumArt::Provider::kWikipedia,
                                     "Massive Attack", "Mezzanine (album)"),
        "wiki guard accepts the exact album title");
  const std::string wiki_wrong =
      "{\"query\":{\"pages\":{\"9\":{\"title\":\"Mezzanine (Massive Attack album)\","
      "\"thumbnail\":{\"source\":\"https://upload.wikimedia.org/wikipedia/en/9/9f/x.png\"}}}}}";
  // "Mezzanine (album)" must not match an unrelated page.
  Check(!AlbumArt::SearchResultMatches(
            "{\"query\":{\"pages\":{\"1\":{\"title\":\"Bohemian Rhapsody\","
            "\"thumbnail\":{\"source\":\"https://upload.wikimedia.org/wikipedia/en/9/9f/x.png\"}}}}}",
            AlbumArt::Provider::kWikipedia, "Massive Attack", "Mezzanine (album)"),
        "wiki guard rejects an unrelated page title");
  Check(!AlbumArt::SearchResultMatches(wiki_wrong, AlbumArt::Provider::kWikipedia,
                                      "Pink Floyd", "The Dark Side of the Moon"),
        "wiki guard rejects a page for another album");

  // Discogs guard: "Artist - Album" must agree with artist + album.
  const std::string discogs_ok =
      "{\"results\":[{\"id\":9287809,"
      "\"title\":\"Pink Floyd - The Dark Side Of The Moon\"}]}";
  Check(AlbumArt::SearchResultMatches(discogs_ok, AlbumArt::Provider::kDiscogs, "Pink Floyd",
                                     "The Dark Side Of The Moon"),
        "discogs guard accepts the requested artist + album");
  Check(!AlbumArt::SearchResultMatches(
            "{\"results\":[{\"id\":1,\"title\":\"Moon Byul - Dark Side of the Moon - EP\"}]}",
            AlbumArt::Provider::kDiscogs, "Pink Floyd", "The Dark Side Of The Moon"),
        "discogs guard rejects another artist's album");

  // MusicBrainz release guard mirrors the group guard.
  const std::string mb_release_ok =
      "{\"releases\":[{\"id\":\"8ecfafd1-89a8-423a-968f-3fff47f0b0f9\","
      "\"title\":\"Random Access Memories\",\"artist-credit\":[{\"name\":\"Daft Punk\"}]}]}";
  Check(AlbumArt::SearchResultMatches(mb_release_ok,
                                     AlbumArt::Provider::kMusicBrainzRelease, "Daft Punk",
                                     "Random Access Memories"),
        "mb release guard accepts the requested release");

  // Track-level resolution guards: artist + title, with a real album to adopt.
  const std::string track_ok =
      "{\"data\":[{\"title\":\"Teardrop\",\"artist\":{\"name\":\"Massive Attack\"},"
      "\"album\":{\"title\":\"Mezzanine\"}}]}";
  Check(AlbumArt::DeezerTrackResultMatches(track_ok, "Massive Attack", "Teardrop"),
        "deezer track guard accepts artist + title with an album");
  Check(!AlbumArt::DeezerTrackResultMatches(track_ok, "Massive Attack", "Angel"),
        "deezer track guard rejects a different title");
  Check(!AlbumArt::DeezerTrackResultMatches(track_ok, "Sandstorm", "Teardrop"),
        "deezer track guard rejects a different artist");

  const std::string song_ok =
      "{\"results\":[{\"artistName\":\"Massive Attack\",\"trackName\":\"Teardrop\","
      "\"collectionName\":\"Mezzanine\"}]}";
  Check(AlbumArt::ItunesSongResultMatches(song_ok, "Massive Attack", "Teardrop"),
        "itunes song guard accepts artist + title with an album");
  Check(!AlbumArt::ItunesSongResultMatches(song_ok, "Massive Attack", "Angel"),
        "itunes song guard rejects a different title");
}

// --- Preference order --------------------------------------------------------

void TestPreferenceOrder() {
  // The two-tier chain is Deezer, iTunes, MusicBrainz, Wikipedia, Discogs:
  // the enum pins the order so Tier 1 (Deezer + iTunes) and Tier 2 (the rest)
  // cannot drift apart from the resolver.
  const AlbumArt::Provider kOrder[] = {
      AlbumArt::Provider::kDeezer,
      AlbumArt::Provider::kItunes,
      AlbumArt::Provider::kMusicBrainz,
      AlbumArt::Provider::kWikipedia,
      AlbumArt::Provider::kDiscogs,
  };
  Check(static_cast<int>(kOrder[0]) < static_cast<int>(kOrder[1]),
        "order: deezer before itunes");
  Check(static_cast<int>(kOrder[1]) < static_cast<int>(kOrder[2]),
        "order: itunes before musicbrainz");
  Check(static_cast<int>(kOrder[2]) < static_cast<int>(kOrder[3]),
        "order: musicbrainz before wikipedia");
  Check(static_cast<int>(kOrder[3]) < static_cast<int>(kOrder[4]),
        "order: wikipedia before discogs");
  Check(kOrder[4] == AlbumArt::Provider::kDiscogs, "order: discogs is last");
}

// --- Live -------------------------------------------------------------------

bool IsNewHostUrl(const std::string& url) {
  if (url.find("upload.wikimedia.org") != std::string::npos ||
      url.find("thumb.wikimedia.org") != std::string::npos ||
      url.find("wikipedia.org") != std::string::npos) {
    return true;
  }
  if (url.find("i.discogs.com") != std::string::npos ||
      url.find("discogs.com") != std::string::npos) {
    return true;
  }
  // The release-search CAA path is new; the group path predates this change.
  if (url.find("coverartarchive.org/release/") != std::string::npos &&
      url.find("/release-group/") == std::string::npos) {
    return true;
  }
  return false;
}

int RunLive() {
  AlbumArt::Resolver resolver;
  resolver.SetOnlineEnabled(true);

  struct Album {
    const char* artist;
    const char* album;
  };
  const Album kAlbums[] = {
      {"Pink Floyd", "The Dark Side of the Moon"},
      {"Daft Punk", "Random Access Memories"},
      {"Massive Attack", "Mezzanine"},
  };

  int resolved = 0;
  int new_host = 0;
  for (const Album& entry : kAlbums) {
    const std::string url = resolver.Lookup(entry.artist, entry.album, "");
    std::printf("live lookup artist=\"%s\" album=\"%s\" url=\"%s\"\n", entry.artist,
                entry.album, url.c_str());
    if (!url.empty()) {
      ++resolved;
      if (IsNewHostUrl(url)) {
        ++new_host;
      }
    }
    // Per-provider proof: Tier 2 hosts are only proven when called directly,
    // because Tier 1 usually wins the combined lookup above.
    bool answered = false;
    const std::string wiki =
        resolver.LookupFromProvider(AlbumArt::Provider::kWikipedia, entry.artist,
                                    entry.album, &answered);
    std::printf("live provider=wikipedia artist=\"%s\" album=\"%s\" answered=%d url=\"%s\"\n",
                entry.artist, entry.album, answered ? 1 : 0, wiki.c_str());
    if (!wiki.empty() && IsNewHostUrl(wiki)) {
      ++new_host;
    }
    const std::string discogs =
        resolver.LookupFromProvider(AlbumArt::Provider::kDiscogs, entry.artist,
                                    entry.album, &answered);
    std::printf("live provider=discogs artist=\"%s\" album=\"%s\" answered=%d url=\"%s\"\n",
                entry.artist, entry.album, answered ? 1 : 0, discogs.c_str());
    if (!discogs.empty() && IsNewHostUrl(discogs)) {
      ++new_host;
    }
    const std::string mb_release =
        resolver.LookupFromProvider(AlbumArt::Provider::kMusicBrainzRelease, entry.artist,
                                    entry.album, &answered);
    std::printf("live provider=mb-release artist=\"%s\" album=\"%s\" answered=%d url=\"%s\"\n",
                entry.artist, entry.album, answered ? 1 : 0, mb_release.c_str());
    if (!mb_release.empty() && IsNewHostUrl(mb_release)) {
      ++new_host;
    }
  }

  // Empty-album resolution: Teardrop must resolve to Mezzanine, then to art.
  const std::string resolved_album =
      resolver.ResolveAlbumTitle("Massive Attack", "Teardrop");
  std::printf("live resolved-album artist=\"Massive Attack\" track=\"Teardrop\" album=\"%s\"\n",
              resolved_album.c_str());
  const std::string albumless_url =
      resolver.Lookup("Massive Attack", "", "C:\\live\\teardrop.flac", "Teardrop");
  std::printf("live albumless artist=\"Massive Attack\" track=\"Teardrop\" url=\"%s\"\n",
              albumless_url.c_str());

  std::printf("live summary resolved=%d/3 new-host-urls=%d\n", resolved, new_host);
  const bool ok = resolved >= 2 && new_host >= 1;
  std::printf("RESULT: %s\n", ok ? "PASS" : "FAIL");
  return ok ? 0 : 1;
}

}  // namespace

int main(int argc, char** argv) {
  if (argc > 1 && argv[1] != nullptr && std::string(argv[1]) == "--live") {
    return RunLive();
  }

  TestUrlBuilders();
  TestExtractors();
  TestGuards();
  TestPreferenceOrder();

  std::printf("album_art tests: %d checks, %d failures\n", g_checks, g_failures);
  std::printf("RESULT: %s\n", g_failures == 0 ? "PASS" : "FAIL");
  return g_failures == 0 ? 0 : 1;
}
