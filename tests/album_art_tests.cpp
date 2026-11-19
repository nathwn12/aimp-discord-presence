
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


void TestUrlBuilders() {
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

  const std::string wiki =
      AlbumArt::BuildWikipediaSearchUrl("Pink Floyd", "The Dark Side of the Moon");
  Check(Contains(wiki, "en.wikipedia.org/w/api.php"), "wiki url targets the api");
  Check(Contains(wiki, "generator=search"), "wiki url searches with a generator");
  Check(Contains(wiki, "prop=pageimages"), "wiki url asks for pageimages");
  Check(Contains(wiki, "pithumbsize=600"), "wiki url asks for 600px thumbs");
  Check(Contains(wiki, "pilicense=any"), "wiki url accepts any license");
  Check(AlbumArt::BuildWikipediaSearchUrl("", "").empty(), "wiki url needs a query");

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

  const std::string all_urls = deezer + deezer_track + itunes + itunes_song + mb_group +
                               mb_release + wiki + discogs;
  Check(!Contains(all_urls, "theaudiodb"), "no builder touches TheAudioDB");
  Check(!Contains(all_urls, "listenbrainz"), "no builder touches ListenBrainz");
  Check(!Contains(all_urls, "audioscrobbler") && !Contains(all_urls, "last.fm"),
        "no builder touches Last.fm");
  Check(!Contains(all_urls, "archive.org"), "no builder touches Internet Archive");
}


void TestExtractors() {
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

  const std::string itunes_body =
      "{\"resultCount\":1,\"results\":[{\"artistName\":\"Massive Attack\","
      "\"trackName\":\"Teardrop\",\"collectionName\":\"Mezzanine\","
      "\"artworkUrl100\":\"https://is1-ssl.mzstatic.com/image/thumb/Music/xyz/100x100bb.jpg\"}]}";
  Check(AlbumArt::ExtractArtworkUrl(itunes_body, AlbumArt::Provider::kItunes) ==
            "https://is1-ssl.mzstatic.com/image/thumb/Music/xyz/600x600bb.jpg",
        "itunes extractor upscales to 600x600");

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

  const std::string deezer_track_body =
      "{\"data\":[{\"title\":\"Teardrop\",\"artist\":{\"name\":\"Massive Attack\"},"
      "\"album\":{\"title\":\"Mezzanine\"}}]}";
  Check(AlbumArt::ExtractDeezerTrackAlbumTitle(deezer_track_body) == "Mezzanine",
        "deezer track extractor returns the album title");

  Check(AlbumArt::ExtractItunesAlbumTitle(itunes_body) == "Mezzanine",
        "itunes song extractor returns the collection name");

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

  Check(AlbumArt::ExtractArtworkUrl(mb_group_body, AlbumArt::Provider::kMusicBrainz).empty(),
        "mb group answer is not an image url");
  Check(AlbumArt::ExtractArtworkUrl(mb_release_body,
                                   AlbumArt::Provider::kMusicBrainzRelease)
            .empty(),
        "mb release answer is not an image url");
}


void TestGuards() {
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

  Check(AlbumArt::SearchResultMatches(deezer_ok, AlbumArt::Provider::kDeezer, "Daft Punk", ""),
        "deezer guard accepts an artist-only hit");

  const std::string wiki_ok =
      "{\"query\":{\"pages\":{\"1791673\":{\"title\":\"Mezzanine (album)\","
      "\"thumbnail\":{\"source\":\"https://upload.wikimedia.org/wikipedia/en/e/e9/x.png\"}}}}}";
  Check(AlbumArt::SearchResultMatches(wiki_ok, AlbumArt::Provider::kWikipedia,
                                     "Massive Attack", "Mezzanine (album)"),
        "wiki guard accepts the exact album title");
  const std::string wiki_wrong =
      "{\"query\":{\"pages\":{\"9\":{\"title\":\"Mezzanine (Massive Attack album)\","
      "\"thumbnail\":{\"source\":\"https://upload.wikimedia.org/wikipedia/en/9/9f/x.png\"}}}}}";
  Check(!AlbumArt::SearchResultMatches(
            "{\"query\":{\"pages\":{\"1\":{\"title\":\"Bohemian Rhapsody\","
            "\"thumbnail\":{\"source\":\"https://upload.wikimedia.org/wikipedia/en/9/9f/x.png\"}}}}}",
            AlbumArt::Provider::kWikipedia, "Massive Attack", "Mezzanine (album)"),
        "wiki guard rejects an unrelated page title");
  Check(!AlbumArt::SearchResultMatches(wiki_wrong, AlbumArt::Provider::kWikipedia,
                                      "Pink Floyd", "The Dark Side of the Moon"),
        "wiki guard rejects a page for another album");

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

  const std::string mb_release_ok =
      "{\"releases\":[{\"id\":\"8ecfafd1-89a8-423a-968f-3fff47f0b0f9\","
      "\"title\":\"Random Access Memories\",\"artist-credit\":[{\"name\":\"Daft Punk\"}]}]}";
  Check(AlbumArt::SearchResultMatches(mb_release_ok,
                                     AlbumArt::Provider::kMusicBrainzRelease, "Daft Punk",
                                     "Random Access Memories"),
        "mb release guard accepts the requested release");

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


void TestPreferenceOrder() {
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
