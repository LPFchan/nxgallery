# NX Gallery

NX Gallery is a Plutonium-based Nintendo Switch homebrew capture browser modeled after the stock Album flow. It scans `sdmc:/Nintendo/Album`, presents a four-column capture grid and viewer, and shares the selected photo or video through a Telegram bot after an explicit destination picker.

## Current prototype

- Read-only recursive album scan for JPEG, PNG, and MP4 captures.
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

The current build host is `yeowoolmac`; the interactive build session is `nxgallery-dev`.

## Install

See [payload/INSTALL.txt](payload/INSTALL.txt). The release archive intentionally contains only the NRO, CA bundle, example configuration, and install guide.
