# NX Gallery Spec

- Project: NX Gallery
- Project id: `nxgallery`
- Operator: LPFchan
- Last updated: 2026-07-21
- Related decisions: DEC-20260717-001

## Thesis

Provide a stock-Album-inspired Nintendo Switch homebrew gallery that lets the operator browse captures and explicitly share one capture or an ordered selection to one Telegram destination through a bot. Multi-select delivery is partitioned into sequential Bot API batches of up to ten captures.

## Core capabilities

- Read captures from `sdmc:/Nintendo/Album` without modifying them.
- Render a controller-first, touch-capable Plutonium grid, viewer, chat picker, sending state, and result state.
- Discover Bot API chats from pending updates, merge configured chats, and persist credential-free chat metadata.
- Play MP4 captures in-app with AAC audio, pause/resume, progress, and left-stick seeking while D-pad navigation continues to change captures.
- Upload only after the operator selects both media and destination; multi-select uploads preserve order and use sequential Bot API requests containing no more than ten captures each.
- Check stable GitHub releases silently and expose an operator-triggered in-app update only when a newer version is available.

## Invariants

- Bot tokens may be read from `/switch/nxgallery/telegram-bot.conf` or the sibling NX Torrent configuration at `/switch/nxtorrent/telegram-bot.conf`; they are never compiled, logged, cached, or packaged.
- TLS peer and host verification remain enabled with the reviewed CA bundle.
- Release updates require the expected public-repository URL, exact NRO asset name, GitHub-provided SHA-256 digest, and a structurally valid NRO before installation.
- Album access is read-only.
- TDLib enters the production boundary only if account-wide chat/history features become required.

## Non-goals for the current slice

- Editing or deleting captures.
- Capture trimming or transcoding.
- Telegram user-account login or complete historical chat enumeration.
