<h1 align="center">AIMP Discord Presence</h1>

<p align="center"><strong>Show what AIMP is playing as a Discord Rich Presence, with the artist, track and matched album art.</strong></p>

<p align="center">
  <img src="https://img.shields.io/badge/platform-windows--64bit-2b3137?logo=windows&logoColor=white" alt="Platform" />
</p>

<p align="center">
  <img src="docs/preview/presence-preview.png" width="720" alt="The Discord profile card showing AIMP playing a track with matched album art" />
</p>

<p align="center">
  <img src=".github/screenshots/1.png" width="190" alt="Presence with artwork and playback time." />
  <img src=".github/screenshots/2.png" width="190" alt="Presence with the track and artist line." />
  <img src=".github/screenshots/3.png" width="190" alt="Presence without a matched artwork." />
  <img src=".github/screenshots/4.png" width="190" alt="Presence paused." />
</p>

## Why AIMP Discord Presence

AIMP has no built-in Discord Rich Presence. This plugin publishes the playing track to your Discord profile the way Spotify does: artist, song and artwork, with optional playback time and configurable badges.

- **Album art from every layer.** Embedded art, a local sidecar and a keyless online lookup are tried in order.
- **A Spotify-shaped card.** The presence shows *Listening to* AIMP with the artwork and track detail.
- **Self-reconnecting.** A dropped connection is retried without restarting AIMP.
- **Config without a window.** Every knob lives in AIMP's ini file, so there is no second UI to learn.

## Install

1. Download `aimp_discordPresence.dll` version 2.2.1 from the [Releases page](https://github.com/nathwn12/aimp-discord-presence/releases), or build it with [docs/BUILDING.md](docs/BUILDING.md).
2. Put it in AIMP's plugin folder — `<AIMP install dir>\Plugins\aimp_discordPresence\` in a standard install (confirm on yours), or use **Settings ▸ Plugins ▸ Install** on the `.dll`.
3. Confirm the plugin is **enabled** in **Settings ▸ Plugins**.
4. Restart AIMP, then play a track; the presence appears on your Discord profile.

## Use it

Play a track in AIMP. The Discord profile card updates on play, pause and track change, and shows the matched artwork when a layer succeeds.

- Playback time is optional; badges are configurable in the ini file.
- If a match fails, the card still shows the track detail without artwork.
- To publish a permanent cover, point `CoverHost` at your own Google Drive; the anonymous hosts work without keys.

## Configure it

There is no settings window. Settings live in AIMP's config file, `Profile/AIMP.ini` (the default location of a standard AIMP install — confirm on your own), in the `[DiscordPresence]` section. AIMP must be **closed** while you edit them.

| Key | Default | Effect |
| --- | --- | --- |
| `UseAlbumArt` | `1` | Use the large image for album art. |
| `LocalCover` | `1` | Publish your own local cover so Discord can fetch it. |
| `UseAlbumArtOnline` | `1` | Keyless online artwork lookup. |
| `CoverHost` | `auto` | `auto`, `drive` (your Google Drive) or `keyless` (anonymous hosts only). |
| `ThrottleMs` | `4000` | Floor between updates, under Discord's five-per-20-seconds cap. |
| `DebugLog` | unset | Write a log such as `discord.log` with the art layers tried. |

One recipe — read why a track has no cover:

```ini
[DiscordPresence]
UseAlbumArt=1
UseAlbumArtOnline=1
DebugLog=discord.log
```

The full key list, Drive setup, privacy notes and a troubleshooting table are in [docs/GUIDE.md](docs/GUIDE.md), section 3.

## Requirements

- Windows x64.
- AIMP, desktop 64-bit build. The plugin is built against the AIMP SDK vendored in `lib/aimp-sdk`.
- The Discord desktop app, running. The browser version will not work; the plugin never launches Discord for you.
- Discord ▸ **Settings ▸ Activity Privacy ▸ "Share your detected activities with others"** must be **on**.
- To build: CMake and the submodules pinned in `.gitmodules`.

## Development

```powershell
git submodule update --init --recursive
cmake -B build -A x64 -DAUTOVERSIONING=ON
cmake --build build --config Release
```

CI runs the same configure and build for Win32 and x64 on every push and pull request. The build script and packaging route are in [docs/BUILDING.md](docs/BUILDING.md).

## Status

**Experimental** — pre-release; interfaces may change.

## Links

- [Issues](https://github.com/nathwn12/aimp-discord-presence/issues)
- [Releases](https://github.com/nathwn12/aimp-discord-presence/releases)
- [Full setup and keys](docs/GUIDE.md)
- [Build from source](docs/BUILDING.md)
- [Exle/aimp-discord-presence](https://github.com/Exle/aimp-discord-presence)

<p align="center"><sub>Built by nathwn12</sub></p>
