# NX Gallery

NX Gallery is a Plutonium-based Nintendo Switch homebrew capture browser modeled after the stock Album flow. On Horizon it enumerates both NAND and SD captures through the `caps:a` Album Accessor service; host tests and Ryujinx fixtures use the filesystem scanner. It presents a four-column capture grid and viewer, and shares the selected photo or video through a Telegram bot after an explicit destination picker.

## Current prototype

- Read-only Horizon Album Accessor enumeration with lazy photo/movie materialization; recursive JPEG, PNG, and MP4 scanning remains the host-test backend.
- Stock-inspired grid, viewer, modal chat picker, sending state, and result dialog.
- Controller and touchscreen targets for captures, Back, Share, chat rows, Send, and Cancel.
- Telegram Bot API chat discovery through `getUpdates`, plus configured and persisted destinations.
- `sendPhoto` and `sendVideo` multipart uploads on a worker thread.
- Bot tokens remain SD-card configuration and are never compiled or packaged.
- Video capture sharing works; in-app video playback is not implemented yet.

## Controls

| Surface | Controls |
| --- | --- |
| Grid | D-pad selects, A opens, + exits |
| Viewer | Left/right changes capture, X opens Telegram share, B returns |
| Chat picker | Up/down selects, A sends, Y refreshes chats, B cancels |

The same surfaces are touch-enabled: tap a capture, then use the visible Back,
Share, chat-row, Send, and Cancel targets.

## Telegram configuration

Copy `telegram-bot.conf.example` to `/switch/nxgallery/telegram-bot.conf` on the SD card and replace its placeholder token. Optionally add `chat=ID|Title` for destinations that must always appear. With `discover_chats=true`, chats observed in pending bot updates are cached at `/switch/nxgallery/telegram-state.json`; the picker reports if that cache cannot be saved.

Bots cannot enumerate every chat they belong to. A chat must be configured or observed in an update. Telegram retains pending updates for no longer than 24 hours. NX Gallery and NX Torrent may sequentially consume the same bot update queue; this is an accepted prototype tradeoff.

For a channel destination, add the bot to the channel as an administrator with
permission to post messages and media. Publish a channel post before refreshing
the picker so Bot API discovery can observe and cache the channel's canonical ID.

TDLib is required only if the product later needs user-account login, account-wide chat enumeration/search, or historical chat access. It is not required for the current bot picker and media upload flow.

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
export PATH="$DEVKITPRO/tools/bin:$DEVKITA64/bin:$PATH"
make -j4
```

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

The production NRO's `--probe` mode attaches stdout and stderr through nxlink,
queries Horizon's NAND and SD Album Accessor stores, refreshes destinations, tries destinations in
channel/private/group/configured order until one accepts a photo, sends the
newest MP4 to that same destination, and prints a final
`NXGALLERY_PROBE_RESULT` line. It does not print the bot token, chat IDs,
titles, or media filenames. Native exceptions and uncaught C++ termination are
mirrored to nxlink and persisted to `/switch/nxgallery/crash.log` (falling back
to `/nxgallery-crash.log` when the app directory is unavailable).

The harness fails unless all required phases report success. For bare nxlink
launches, it transfers the credential once over ephemeral TLS; the NRO verifies
the server's public-key fingerprint itself before requesting the credential.
The token is kept in memory and never appears in nxlink arguments, output, or
the NRO.

## Install

See [payload/INSTALL.txt](payload/INSTALL.txt). The release archive intentionally contains only the NRO, CA bundle, example configuration, and install guide.
