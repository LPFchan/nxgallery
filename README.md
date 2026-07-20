# NX Gallery

NX Gallery is a Plutonium-based Nintendo Switch homebrew capture browser modeled after the stock Album flow. On Horizon it enumerates both NAND and SD captures through the `caps:a` Album Accessor service; host tests and Ryujinx fixtures use the filesystem scanner. It presents a four-column capture grid and viewer, and shares one capture or an ordered selection through a Telegram bot after an explicit destination picker. Large selections are sent sequentially in Telegram batches of up to ten.

## Current prototype

- Read-only Horizon Album Accessor enumeration with lazy photo/movie materialization; recursive JPEG, PNG, and MP4 scanning remains the host-test backend.
- Stock-inspired grid, viewer, modal chat picker, sending state, and result dialog.
- Controller and touchscreen targets for captures, Back, Share, chat rows, Send, and Cancel.
- One asynchronous Telegram Bot API refresh at launch, plus configured and persisted destinations; opening Share reads the in-memory cache immediately.
- `sendPhoto`, `sendVideo`, and mixed-media `sendMediaGroup` multipart uploads on a worker thread with colored whole-selection transfer progress and B-button cancellation; selections larger than ten are partitioned into ordered Bot API requests.
- Bot tokens remain SD-card configuration and are never compiled or packaged.
- Video captures use Album Accessor JPEG thumbnails. FFmpeg-backed playback includes AAC audio, pause/resume, elapsed progress, and five-second left-stick seeking.

## Controls

| Surface | Controls |
| --- | --- |
| Grid | D-pad moves, A opens, X shares the current capture, + enters multi-select; in multi-select A marks captures and X shares the selection in ordered Telegram batches of up to ten. When a newer release is available, Minus or the bottom-left Minus Update button installs it. |
| Viewer | D-pad left/right changes capture, A plays/pauses video, left-stick left/right seeks five seconds while playing, X opens Telegram share, B returns; videos show elapsed and total playback progress |
| Chat picker | Up/down selects, A sends, Y refreshes chats, + opens Bot Setup, B cancels |
| Sending | B aborts the active Telegram transfer |

The same surfaces are touch-enabled: tap a capture, then use the visible Back,
Share, chat-row, Send, and Cancel targets.

## Telegram configuration

When neither NX Gallery nor NX Torrent has a bot token on the SD card, NX
Gallery opens the full-screen Telegram onboarding flow at launch. The console starts a one-shot
HTTP listener on the local network and shows a QR code (plus the same URL as
text). Scanning it on a phone that shares the Switch's Wi-Fi opens a paste-the-
token form served by the console itself; submitting writes
`/switch/nxgallery/telegram-bot.conf` and starts chat discovery immediately.
The URL contains a random one-shot path, the listener accepts one valid token
and closes, and the token never leaves the local network. Networks with client
isolation (hotels, campus Wi-Fi) block phone-to-console traffic; use the manual
path below there.

NX Gallery also reads the token from
`/switch/nxtorrent/telegram-bot.conf`, so an existing NX Torrent setup skips
onboarding. Each app keeps its own destination and runtime state. Open the
Telegram share sheet and press + whenever Bot Setup needs to be run again.

Manually: copy `telegram-bot.conf.example` to `/switch/nxgallery/telegram-bot.conf` on the SD card and replace its placeholder token. Optionally add `chat=ID|Title` for destinations that must always appear. With `discover_chats=true`, chats observed in pending bot updates are cached at `/switch/nxgallery/telegram-state.json`. NX Gallery performs one background refresh at launch; opening the picker never waits for Telegram. Y starts another background refresh.

Bots cannot enumerate every chat they belong to. A chat must be configured or observed in an update. Telegram retains pending updates for no longer than 24 hours. NX Gallery and NX Torrent may sequentially consume the same bot update queue; this is an accepted prototype tradeoff.

For a channel destination, add the bot to the channel as an administrator with
permission to post messages and media. Publish a channel post before refreshing
the picker so Bot API discovery can observe and cache the channel's canonical ID.

TDLib with a user-account login is required for account-wide chat enumeration/search or historical access. Bot-authorized TDLib, including the proven NX Torrent Horizon port, remains update-only and cannot reconstruct missing bot chat history.

## Updates

NX Gallery checks the latest stable release from `LPFchan/nxgallery` once at
startup. The check stays invisible when the installed version is current, when
no release exists, or when GitHub cannot be reached. A newer release adds a
Minus Update action to the bottom-left of the grid; press Minus or tap that
button to download and install it. Download progress uses the same panel and
transition as Telegram transfers. The app verifies GitHub's SHA-256 asset digest
and the NRO structure before replacing the launched NRO, and keeps the previous
executable with a `.previous` suffix for rollback. Restart NX Gallery after a
successful update.

Published releases must use a stable `vMAJOR.MINOR.PATCH` tag, build with the
matching `APP_VERSION`, and attach the production executable with the exact
asset name `nxgallery.nro`. Prereleases and differently named assets are not
offered in-app.

## Build

Host policy tests:

```sh
make host-test
```

Switch build variables mirror the proven NX Torrent toolchain:

```sh
export DEVKITPRO=/opt/devkitpro
export DEVKITA64="$DEVKITPRO/devkitA64"
export PORTLIBS="$DEVKITPRO/portlibs/switch"
export PLUTONIUM_PREFIX=/path/to/staged/plutonium
export SWITCH_CURL_PREFIX=/path/to/staged/switch-curl
export SWITCH_OPENSSL_PREFIX=/path/to/staged/switch-openssl
export PLAYBACK_PREFIX=/path/to/staged/switch-ffmpeg-portlibs
export PATH="$DEVKITPRO/tools/bin:$DEVKITA64/bin:$PATH"
make -j4 APP_VERSION=0.1.2
```

`PLAYBACK_PREFIX` must contain a Switch FFmpeg build with H.264 and AAC
decoders. Build a compatible FFmpeg 7.1 Switch source tree into a fresh prefix
before building NX Gallery:

```sh
scripts/build-switch-ffmpeg.sh /path/to/ffmpeg-7.1 /path/to/build /path/to/prefix
export PLAYBACK_PREFIX=/path/to/prefix
```

The Makefile rejects playback prefixes that do not register the AAC decoder.

For Ryujinx UI verification, `make automation` produces a separate
`nxgallery-automation.nro`. It advances grid → viewer → chat picker without
sending media. This profile is for emulator QA only; release packages use
`nxgallery.nro`.

The current build host is `yeowoolmac`; hardware probe work runs in
`main:nxgallery-probe`.

## Automated hardware probe

With NetLoader active on the Switch and a credential file at
`.secrets/telegram-bot.conf` (or `PROBE_CONFIG=/path/to/file`), run:

```sh
scripts/run-hardware-probe.sh SWITCH_IP
```

The default probe is non-sending: it validates album access, real MP4 decode,
pause/resume, networking, and destination refresh without posting media. A
one-shot delivery probe must be explicitly requested with
`NXGALLERY_PROBE_SEND_MEDIA=1`; never wrap that mode in an automatic retry loop.

To launch the production UI with the same memory-only credential transfer:

```sh
NXGALLERY_RUN_MODE=interactive scripts/run-hardware-probe.sh SWITCH_IP
```

The production NRO's `--probe` mode attaches stdout and stderr through nxlink,
queries Horizon's NAND and SD Album Accessor stores, decodes a real MP4 and verifies that pause freezes its frame counter and resume advances it, refreshes destinations, and prints a final
`NXGALLERY_PROBE_RESULT` line. It does not print the bot token, chat IDs,
titles, or media filenames. Native exceptions and uncaught C++ termination are
mirrored to nxlink and persisted to `/switch/nxgallery/crash.log` (falling back
to `/nxgallery-crash.log` when the app directory is unavailable).
The harness retains its sanitized last-run transcript at
`artifacts/hardware-probe-last.log` and reports a distinct failure when hbmenu
NetLoader never accepts the NRO.

The harness fails unless all required phases report success. For bare nxlink
launches, it transfers the credential once over ephemeral TLS; the NRO verifies
the server's public-key fingerprint itself before requesting the credential.
The token is kept in memory and never appears in nxlink arguments, output, or
the NRO.

## Install

See [payload/INSTALL.txt](payload/INSTALL.txt). The release archive intentionally contains only the NRO, CA bundle, example configuration, and install guide.
