# The full setup guide

This is the long-form, start-to-finish guide for someone who is not the author: what the
plugin needs, how to install it, and how to stand up your own permanent cover host so the
artwork in your Discord presence is your own.

Every command below is copy-pasteable in Windows PowerShell. Nothing here needs a build
unless you want one — a release DLL works.

---

## 1. What you are setting up

AIMP Discord Presence speaks Discord's local Rich Presence protocol and tells Discord what
AIMP is playing. Discord then draws the card **itself**, from the fields the plugin sends.

That single fact decides the hard requirement of this setup:

> **Discord renders an image the plugin sends, so a cover must be reachable as a public
> HTTPS image URL.** A file path on your disk is useless to Discord; it cannot read
> `D:\Music\cover.jpg`.

So the plugin does not "hand Discord a picture". It:

1. finds a cover locally (art embedded in the file's tags, or a conventional sidecar image
   next to the track),
2. uploads the image bytes somewhere public and gets back an `https://…` URL,
3. **proves that URL works** — a bare GET with no credentials must answer HTTP 200 with an
   `image/*` content type — and only a URL that passed is ever used,
4. in parallel, when online lookups are allowed, asks keyless music services for a cover URL
   (a published local cover still wins for that track),
5. if neither produced a URL, the card carries a built-in placeholder image.

Three things follow from step 3, and they explain most of the troubleshooting later:

- the host must serve the image **anonymously** (no login), which is why the Google Drive
  folder in section 4 is shared publicly once, and why the keyless hosts serve anonymous
  URLs;
- a keyless URL can die, so a cached catbox, uguu or litterbox URL is re-checked before it
  is reused — and re-published to Drive when Drive is in the chain. A cached **Drive** URL
  is reused as-is, so if one ever stops working the cache entry has to be cleared by hand
  (section 7.3). A cached URL from the removed GitHub host is never reused as-is: with
  Drive in the chain it is upgraded to Drive, otherwise it is re-checked like a keyless URL;
- the plugin never trusts a host's "OK" — the health check is the gate.

Nothing is installed system-wide, no service runs, and there is no settings window.

---

## 2. Install the plugin

### 2.1 What you need

- Windows x64.
- AIMP, desktop build, 64-bit.
- The Discord **desktop** app running. The browser client cannot serve the local IPC pipe
  the plugin talks to.
- In Discord: **Settings ▸ Activity Privacy ▸ "Share your detected activities with others"**
  turned **on**.

### 2.2 Where the DLL goes

Build output (if you build it yourself):

```
output\Release\aimp_discordPresence\x64\aimp_discordPresence.dll
```

On a **standard AIMP install** the installer places the plugin in a per-plugin subfolder of
AIMP's `Plugins` folder. That layout is AIMP's, not something this repository defines — if the
folder is not where this path says, find your own `Plugins` folder and use it:

```
<AIMP install directory>\Plugins\aimp_discordPresence\aimp_discordPresence.dll
```

You do not have to know that path: AIMP's own installer route creates it for you. This
plugin is not in AIMP's official catalogue, so it is always installed by hand.

### 2.3 Steps

1. Close AIMP.
2. Either copy the DLL into the `Plugins\aimp_discordPresence\` folder of your install (the
   path above, confirmed on your machine), or open AIMP and use **Settings ▸ Plugins ▸
   Install**, then point AIMP at the `.dll` (or at a release archive).
3. In **Settings ▸ Plugins**, confirm **Discord Presence** is listed and **enabled**.
4. Start AIMP and play a track.
5. Check your Discord profile: the card should appear under *Listening to* AIMP.

If AIMP never lists the plugin, the DLL did not load — check that it is the **x64** build;
a 32-bit DLL is silently ignored by 64-bit AIMP.

### 2.4 Verify the presence before touching covers

Get the plain card working first. If the card does not appear at all, cover art is not your
problem yet — go to the troubleshooting table and fix the Discord toggle or the client
first. Covers are an enhancement on top of a working presence.

### 2.5 Building from source

Only needed if no release fits your AIMP version. The repository builds with CMake and a
submodule; the exact steps, prerequisites and test commands live in
[docs/BUILDING.md](BUILDING.md).

---

## 3. Put the Drive settings into AIMP.ini

### 3.1 Where the file is

Configuration is ini-only. On a standard AIMP install it lives in AIMP's profile, in
`Profile/AIMP.ini`, under the section **`[DiscordPresence]`** — that path is AIMP's default
layout, not something this repository proves, so confirm it on your own install (AIMP's
settings show you where the Profile folder is) if the file is not there.

> **Close AIMP before editing.** AIMP writes `AIMP.ini` on exit and does not pick up edits
> made while it is running. Edit → save → start AIMP.

### 3.2 The cover keys

| Key | Type | Default | What it does |
| --- | --- | --- | --- |
| `UseAlbumArt` | bool (`0`/`1`) | `1` | Turns the large image on. When off, neither the local cover nor the online result is used, so the card keeps the placeholder — and nothing is uploaded. |
| `UseAlbumArtOnline` | bool (`0`/`1`) | `1` | Allows the keyless online artwork lookup (Deezer album → iTunes album → MusicBrainz + Cover Art Archive → Wikipedia → Discogs). `0` = never go online. |
| `LocalCover` | bool (`0`/`1`) | `1` | Publishes the track's own cover (embedded art or sidecar file) to an upload host so Discord can fetch it. |
| `CoverCache` | string | empty → `cover-cache.txt` beside the DLL | File that remembers published URLs by image hash. A bare filename resolves next to the plugin DLL; an absolute path is used as given. |
| `CoverHost` | `auto` / `drive` / `keyless` | `auto` | Which host publishes the covers. `auto` uses Drive when `DriveClientId` and `DriveClientSecret` are both set, otherwise the keyless hosts; `drive` and `keyless` pin the preference, and the keyless hosts always remain the last resort. Case-insensitive, and any other value is read as `auto`. |
| `DriveClientId` | string | empty | Your Google OAuth **Desktop app** client ID. Empty keeps Drive out of the chain entirely — the setup is in section 4. |
| `DriveClientSecret` | string | empty | The matching client secret (`GOCSPX-…`). Stored in plaintext in this file, like any other key here. |
| `DebugLog` | string | empty (off) | Optional log file for the IPC frames and the cover layer/host decisions. This is the file you read when a cover does not appear. |

Old GitHub cover keys left in an existing config from a previous version are ignored
without error: GitHub cover storage is unwired, they enable no host, and they reach no
network. Google Drive is the only permanent host.

Everything else — card text templates, buttons, badges, `ThrottleMs`, `StatusDisplayType` —
is documented in [docs/BUILDING.md](BUILDING.md). None of it affects covers.

### 3.3 A complete cover configuration

```ini
[DiscordPresence]
UseAlbumArt=1
UseAlbumArtOnline=1
LocalCover=1
CoverHost=auto
DriveClientId=1234567890-abc123.apps.googleusercontent.com
DriveClientSecret=GOCSPX-xxxxxxxxxxxxxxxx
CoverCache=cover-cache.txt
DebugLog=cover.log
```

Both Drive values are placeholders — paste the Client ID and the secret from section 4.
Never paste a real secret into a guide, an issue, a chat, or a commit.

Two failure modes worth knowing before you restart:

- **A misspelt `CoverHost`** reads as `auto`: the chain still works, it just does not pin
  what you meant.
- **Missing Drive keys** leave Drive out of the chain: the plugin publishes to the keyless
  hosts only. The startup line `local-cover hosts policy=… drive=off drive-reason=…` in the
  log names the cause (`no-credentials`, `no-client-id`, `no-client-secret`).

### 3.4 The upload host chain

Order, exactly as tried. First usable URL wins, and the winner must pass the health check.

Google Drive is the only permanent host. Under the default `auto` the plugin puts
**Drive** in the chain when `DriveClientId` and `DriveClientSecret` are both set, and
publishes to the keyless hosts otherwise. `CoverHost=drive` pins Drive (and falls through
to the keyless hosts when Drive is not configured or fails); `CoverHost=keyless` keeps the
anonymous hosts only. The keyless hosts are always appended last, whatever the preference.

| # | Host | Endpoint | Notes |
| --- | --- | --- | --- |
| 1 | `drive.google.com` | `POST https://www.googleapis.com/upload/drive/v3/files?uploadType=multipart` | Only present when both Drive keys are set (section 4). Files land in your own Drive; the public URL is `https://lh3.googleusercontent.com/d/<id>`. |
| 2 | `catbox.moe` | `POST https://catbox.moe/user/api.php` | Keyless, permanent. Always tried. |
| 3 | `uguu.se` | `POST https://uguu.se/upload?output=text` | Keyless. URLs are ephemeral. |
| 4 | `litter.catbox.moe` | `POST https://litterbox.catbox.moe/resources/internals/api.php` | Keyless, 72-hour retention, kept last as a dormant fallback. |

All four are the same layer: a host failure moves to the next host, and a host failure never
skips the online layer — it simply means this track has no published local cover, so the
online URL (when one was found) is shown instead of a blank card.

The online lookup is a separate, also keyless chain. It runs when `UseAlbumArt=1` and
`UseAlbumArtOnline=1`, in parallel with the local publish, and a published local cover for
the same track takes precedence over its result. Tried in order, first guarded hit wins:

| # | Provider | What is queried |
| --- | --- | --- |
| 1 | `api.deezer.com` | Album search for artist + album; artwork `cover_xl` |
| 2 | `itunes.apple.com` | Album search for artist + album (artwork upscaled to 600×600) |
| 3 | `musicbrainz.org` → `coverartarchive.org` | Release-group search for an MBID, then the front cover; on a miss the release search is tried, then the release cover (`/release/<mbid>/front-500`) |
| 4 | `wikipedia.org` | Page images (`generator=search`, thumbnails at 600 px) for the release, with an exact-title guard |
| 5 | `discogs.com` | Database search, then `GET /releases/<id>` for the first release image |

No API keys, accounts or application registration are involved anywhere in this chain. Every
hit passes an identity guard (requested artist + album must agree with the hit, ignoring
case, punctuation and whitespace), and a rejected hit falls through to the next provider.
Deezer and iTunes run together in a first tier and win whenever either hits; only when both
miss do MusicBrainz, Wikipedia and Discogs run together in a second tier.

When the album tag is empty, the track title resolves the album first: Deezer track search
(strict artist + track syntax) is tried, then iTunes song search (`entity=song`), each
guarded by artist + title, and the returned album title becomes the effective album for the
lookup above. When neither resolves, the lookup falls back to an artist-only search.

---

## 4. Use your own Google Drive as the cover host

Google Drive is the only permanent cover host: the plugin uploads the cover into a folder
there, and the card points at Google's image link for that file. With both Drive keys set and
`CoverHost=auto`, Drive is the one used (section 3.4); without them the keyless hosts carry
every upload.

### 4.1 Why Drive

The covers sit in **your own Drive**, under **your own storage quota**, and you can see, share or
delete each one like any other file in Drive. The plugin creates one folder, makes it publicly
readable **once**, and every cover it uploads there inherits that read access — there is no
per-file sharing call and nothing to maintain per track. A published cover is addressed as
`https://lh3.googleusercontent.com/d/<file-id>`, which Discord renders as presence art; the
plugin builds exactly that link.

The cost is the setup below: a Google Cloud project and an OAuth client of your own. It is
longer than pasting a key, and it has one browser step per machine.

### 4.2 One-time Google setup

Nothing here is per track; do it once.

1. **Create a project and enable the Drive API.**
   - <https://console.cloud.google.com/projectcreate> → **Project name** (anything, e.g.
     `aimp-covers`) → **Create**.
   - <https://console.cloud.google.com/apis/library/drive.googleapis.com> → **Enable**.
2. **Fill in the OAuth consent screen, then publish it.**
   - <https://console.cloud.google.com/apis/credentials/consent> → **User type: External** →
     **Create**.
   - **App name**, **User support email**, **Developer contact information**: fill all three,
     then **Save and continue** through **Scopes** and **Test users**. Leave both empty — the
     plugin requests its one scope itself, `.../auth/drive.file`.
   - Back on the consent-screen page: **Publish app** → **Confirm**, so **Publishing status**
     reads **In production**.
3. **Create a desktop client and copy its two values.**
   - <https://console.cloud.google.com/apis/credentials> → **Create credentials** →
     **OAuth client ID**.
   - **Application type: Desktop app** → **Create**.
   - Copy the **Client ID** and the **Client secret** (the secret is the `GOCSPX-…` value).

> **Publish the app. Do not leave it in *Testing*.** In *Testing*, Google expires the refresh
> token after **7 days**. The plugin then loses Drive quietly: no line on the card says why, the
> Drive uploads simply stop, and on a later track the browser asks you to consent again. In
> *In production*, the refresh token keeps working.

### 4.3 The Drive keys

Set these three keys in `[DiscordPresence]` — the table in section 3.2 describes every key:

```ini
[DiscordPresence]
UseAlbumArt=1
UseAlbumArtOnline=1
LocalCover=1
CoverHost=auto
DriveClientId=1234567890-abc123.apps.googleusercontent.com
DriveClientSecret=GOCSPX-xxxxxxxxxxxxxxxx
CoverCache=cover-cache.txt
DebugLog=cover.log
```

Both Drive values are placeholders — paste the Client ID and the secret from step 3.
`CoverHost=auto` is enough: Drive is used automatically once both are set. Use
`CoverHost=drive` to pin Drive, or `keyless` to ignore it without deleting the keys; any
other value is read as `auto`.

Editing the config has the same rule as everywhere else: **edit while AIMP is closed**. AIMP
writes `AIMP.ini` on exit and ignores edits made while it runs, so edit → save → start AIMP.

### 4.4 First run: the consent tab

Play a track that has local art (embedded art, or a `cover.jpg` beside it). On the first publish
the plugin opens your **default browser** once and listens on a loopback port for the answer, for
up to two minutes:

1. Google asks which account to use — pick the one that owns the Drive.
2. Google shows the app's consent screen ("AIMP Discord Presence wants to access your Google
   Account"). Allow it. The request is `drive.file`, which lets the plugin create and manage only
   the files it made itself.
3. When the tab reads **"Google Drive is connected. You can close this tab."** the consent
   worked. Close it.

What success looks like:

- the tab shows that line;
- the log (`DebugLog`) has `local-cover drive auth=consent port=…` and then
  `local-cover drive auth=consented`;
- the refresh token is stored in `drive-token.txt` next to `cover-cache.txt`, so this browser
  step does not repeat while that file and the app's publishing status survive;
- the `local-cover published host=…` line names `drive.google.com` and the URL is an
  `lh3.googleusercontent.com` link.

If you close the tab without allowing, the log says `auth=declined` and the plugin does not open
a browser again for ten minutes; the cover falls to the next host in the chain meanwhile.

### 4.5 Where the covers live

The plugin creates the folder itself, on the first successful publish, in the root of your Drive:

```
My Drive/
└── aimp-covers/
    └── <sha256-of-uploaded-bytes>.jpg
```

- The name is the SHA-256 of the bytes actually uploaded, so the same image is stored once.
- The folder is shared **once**, as *anyone with the link · Viewer*; every file uploaded into it
  inherits that. The plugin never shares an individual cover.
- **Do not seed the folder by hand.** The plugin's access is the `drive.file` scope, which only
  reaches files and folders the app itself created. A cover you copy into `aimp-covers` is
  invisible to the plugin — it is never found, never uploaded and never turned into a URL — and a
  folder you create yourself is not the one it uses. Hand-placed art does nothing.
- Deleting a cover in Drive breaks that cover's URL, and the cache keeps the dead link. Clear
  the `cover-cache.txt` entry the same way as for a deleted keyless URL (section 7.3).

### 4.6 Privacy

With Drive the cover image is stored in **your own Drive**, not on a third-party host; the online
lookup still sends the artist and album tags as search terms when `UseAlbumArtOnline=1`
(section 5.2), exactly as before. When the album tag is empty, the track title is sent instead,
as the Deezer track / iTunes song search terms.

Drive-specific failures and their fixes are rows in the troubleshooting table (section 6).

---

## 5. What gets uploaded, and privacy

### 5.1 What leaves your machine

When a local cover is published, the upload request carries:

- **the image bytes** — nothing else in the payload;
- for the keyless hosts, a multipart form whose only fields are `reqtype` (plus `time=72h` for
  litterbox), a fixed filename (`cover.png` / `cover.jpg` chosen from the MIME type) and the
  content type;
- for Drive, a `multipart/related` body whose JSON part carries only the file name (the image
  hash) and the `aimp-covers` folder id, then the image bytes.

So, plainly: **no track title, no artist, no album name, no file path, and no AIMP data is
sent to any upload host.** The file name Drive receives is the SHA-256 of the uploaded
bytes; the keyless hosts assign their own name to the file.

Before upload the image is centre-cropped to a square and scaled to at most 512 px on the
longest side, then re-encoded as JPEG at quality 85 — unless it is already at most 512 px on
both sides and at most 256 KB, in which case it goes up untouched. If the encoder fails, the
original bytes are sent instead. A local cover larger than 20 MiB is skipped before the
publisher ever sees it. Only PNG and JPEG local art is uploaded at all; other formats are
logged as `unsupported-format` and skipped.

### 5.2 What does leave your machine, but not through the cover upload

Be clear about the rest of the picture:

- **The online lookup is not private.** With `UseAlbumArtOnline=1` (the default) the plugin
  sends **the artist and album tag values** as search terms to Deezer, iTunes, MusicBrainz,
  Wikipedia and Discogs — on every track change, whether or not a local cover exists or was
  published. When the album tag is empty, the **track title** is sent instead, as the Deezer
  track / iTunes song search terms. A local cover does not suppress those requests; it only
  wins the card once it has been published. Setting `UseAlbumArtOnline=0` stops those
  requests entirely: no lookup is made at all, and the card can then only use a local cover
  (published to an upload host).
- **Discord receives the presence itself** — the title, artist, album and cover URL shown on
  the card. That is the product, and it is what Rich Presence means.
- **Local files.** `cover-cache.txt` holds one line per image, either
  `<sha256> ok <url>` or `<sha256> fail <expiry> <reason>`. It contains hashes, URLs and
  host/status reasons — no titles, no paths. `DebugLog`, if you enable it, *does* contain
  track metadata (it logs the card it sends), so treat it as a private file: delete it when
  you are done debugging.

### 5.3 What is never uploaded

- The audio file.
- The track's tags or path (except as the online-lookup search terms described above).
- Your Drive client secret, to anything other than Google's token endpoint.
- Any Discord credential. The plugin has none: it talks to the local Discord IPC pipe.

---

## 6. Troubleshooting

| Symptom | Likely cause | Fix |
| --- | --- | --- |
| No presence at all | Discord's activity sharing is off, the browser client is in use, or AIMP was not restarted | Discord ▸ Settings ▸ Activity Privacy ▸ enable "Share your detected activities with others"; run the desktop app; restart AIMP |
| AIMP does not list the plugin at all | 32-bit DLL, or the file is not in AIMP's plugin folder | Use the x64 build; reinstall through Settings ▸ Plugins ▸ Install |
| Presence works, no cover on one album | Every layer missed for that track | Enable `UseAlbumArt=1` + `UseAlbumArtOnline=1`; set `DebugLog` and read the `local-cover` lines |
| Drive uploads stop after about a week, and the browser asks for consent again | The OAuth app slipped back to **Testing**, where Google expires the refresh token after 7 days | Republish it: <https://console.cloud.google.com/apis/credentials/consent> → **Publish app**, until **Publishing status** reads **In production**; restart AIMP |
| Drive covers stop rendering, the health check fails, and the log shows `http-403` | The `aimp-covers` folder was made private again, so the `lh3` link is no longer anonymously fetchable | In Drive, share `aimp-covers` → **Anyone with the link** → **Viewer**; restart AIMP |
| The consent tab never opens on the first Drive track | `CoverHost` is `keyless`, or `DriveClientId`/`DriveClientSecret` is missing or misspelt | Read `DebugLog`: the startup line `local-cover hosts policy=… drive=off drive-reason=…` names the cause (`policy`, `no-credentials`, `no-client-id`, `no-client-secret`) |
| Repeat plays of the same track upload nothing | Expected: the cache reuses the published URL, keyed by the image hash | Nothing to do |
| Cover upload failed, then art appears anyway | The Drive host failed and a keyless host won | Nothing to do. Check the log's `host=` field to see who served it |
| Cover is a placeholder even though the album exists online | The provider's hit failed the identity guard, or the online lookup is off | Confirm `UseAlbumArtOnline=1`; the log names the provider tried |
| Online lookup returns nothing for an obscure release | Deezer/iTunes/MusicBrainz/Wikipedia/Discogs simply do not carry it | Add the art to the file's tags or drop a `cover.jpg` next to the track — a local cover is then published by your upload chain (permanently, with Drive set up) |
| Image "rejected" (no cover, `unsupported-format` or `too-large` in the log) | Local art is not PNG/JPEG, or is over 20 MiB | Convert the art to PNG/JPEG; 20 MiB is a hard skip, not a resize |
| `local-cover cache-stale` in the log, a cover that used to work stops working | A cached URL on an ephemeral keyless host died | Expected: with Drive in the chain the plugin upgrades to Drive; without it re-checks and re-uploads |
| Presence does not refresh after an edit | `AIMP.ini` was edited while AIMP was running | Close AIMP, edit, save, start AIMP |
| Updates lag a few seconds behind track changes | `ThrottleMs` (default 4000 ms) spaces updates under Discord's 5-per-20-seconds cap | Lower `ThrottleMs` only if you accept the cap; `0` disables the plugin's own limit |
| MusicBrainz lookups feel slow | MusicBrainz allows about one request per second, and the plugin honours it | Nothing to fix; the gate is deliberate |

### 6.1 Reading the log

Set `DebugLog=cover.log` and the file lands beside the plugin DLL. Lines to grep:

- `local-cover hosts policy=… drive=…` — which hosts are in the chain for this load.
- `local-cover published host=… url=…` — which host won.
- `local-cover failed host=… reason=…` — every host failed; `reason` and the host list are
  in the line.
- `local-cover skipped … reason=unsupported-format` / `reason=too-large`.
- `local-cover drive auth=consent …` / `auth=consented` / `auth=declined` — the Drive consent
  flow.
- `large-image applied … source=…` — which layer the card actually used.

---

## 7. Uninstall, disconnect, delete

### 7.1 Remove the plugin

1. Close AIMP.
2. In **Settings ▸ Plugins**, disable **Discord Presence** (or use AIMP's uninstall action if
   it offers one).
3. Delete the plugin folder you installed into — the `Plugins\aimp_discordPresence\` folder of
   your AIMP install, as located in section 2.
4. Delete leftover files next to the old DLL if you enabled them: `cover-cache.txt` and your
   `DebugLog` file. To disconnect Drive as well, delete `drive-token.txt` beside them: it
   holds the refresh token and the folder id, and without it the plugin asks for consent
   again on the next Drive publish.
5. Optionally remove the `[DiscordPresence]` section from your config file (`Profile/AIMP.ini` by
   default). This is where the **Drive client secret is stored in plaintext**, so removing it
   is the thorough option.

### 7.2 Disconnect Drive

Delete `drive-token.txt` next to `cover-cache.txt` and blank `DriveClientSecret=`. Deleting
the file alone is enough to stop Drive uploads (the next publish re-opens the consent tab
instead); blanking the secret keeps the plugin from asking again.

### 7.3 Delete the Drive folder (optional)

Deleting `aimp-covers` in Drive breaks every cover URL already published there, so old cards
stop rendering their cover. A cached Drive URL is reused as-is — the plugin only re-checks
cached URLs from the keyless hosts — so those covers stay broken until the cache entry goes.
The manual recovery is one of:

- delete the offending `<sha256> ok <url>` line from `cover-cache.txt` beside the plugin DLL, or
- delete `cover-cache.txt` entirely and let it be rebuilt,

then restart AIMP. Delete the folder only if you want the covers gone; a Drive folder of
hash-named images leaks no listening history by itself, though the *set* of covers is visible
to anyone with the link.

### 7.4 What uninstalling does not clean up

Keyless-host uploads (catbox, uguu, litterbox) are **not** yours to delete — they were
anonymous. Catbox files are permanent; litterbox expires after 72 hours; uguu URLs are
ephemeral. There is no delete call in the plugin for them.

---

## 8. FAQ

**Does it work with only keyless hosts, no Drive?**
Yes. Empty Drive keys are a supported configuration, not a degraded one: the Drive host is
skipped and catbox, uguu and litterbox are used. You lose permanence and control — a keyless
URL can expire — and the plugin compensates by re-checking a cached keyless URL before reusing
it, or re-publishing it to Drive when Drive is in the chain. Cached Drive URLs get no such
check (section 7.3), which only matters if one of those ever dies. Set up Drive if you want
artwork that keeps working for years.

**Does it upload every track?**
No. It uploads only when there is a **local** cover to publish (art embedded in the tags, or
a sidecar image), and only when `UseAlbumArt=1` and `LocalCover=1`. An album whose art came
from the online lookup is not uploaded at all — Discord fetches the provider's URL directly.
Identical images are uploaded once: the hash is remembered for the session and in
`cover-cache.txt`.

**What if the online lookup finds nothing?**
The card stays on whatever the local layers produced, and shows the plugin's built-in
placeholder image only when they produced nothing either. Since the online providers are
searched by artist and album, a release they do not carry will never resolve — drop a
`cover.jpg` beside the track (or tag the art) and the local layer publishes it instead.

**Do I need to keep `DebugLog` on?**
No. It is for diagnosing. It does contain track metadata, so turn it off when you are done.

**Will someone see my listening history in the Drive folder?**
What is stored is a set of hash-named JPEGs. A visitor cannot tell which track a file came
from except by looking at the image. They can see *which covers exist* and when each was
added, because the folder has no other content. Unsharing the folder does not hide them
either: the health check then fails and the same covers are published to the
keyless hosts instead — anonymously, and temporarily. The only way to publish nothing is to
set `LocalCover=0`.

**Can I use my own Discord application ID instead of the default?**
The card's application ID is a separate key, `ApplicationID`, defaulting to the shared
upstream application. Changing it means the card shows your application's name and needs
Rich Presence enabled on that application — see [docs/BUILDING.md](BUILDING.md).

**Is this plugin in AIMP's official catalogue?**
No. It is installed by hand, as in section 2.

**Does it work without Discord running?**
No. The presence is meaningless without a client to render it; the plugin never launches
Discord for you and simply has nothing to talk to.
