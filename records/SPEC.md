# NX Gallery Spec

- Project: NX Gallery
- Project id: `nxgallery`
- Operator: LPFchan
- Last updated: 2026-07-17
- Related decisions: DEC-20260717-001

## Thesis

Provide a stock-Album-inspired Nintendo Switch homebrew gallery that lets the operator browse captures and explicitly share one photo or video to one Telegram destination through a bot.

## Core capabilities

- Read captures from `sdmc:/Nintendo/Album` without modifying them.
- Render a controller-first, touch-capable Plutonium grid, viewer, chat picker, sending state, and result state.
- Discover Bot API chats from pending updates, merge configured chats, and persist credential-free chat metadata.
- Upload only after the operator selects both media and destination.
- Check stable GitHub releases silently and expose an operator-triggered in-app update only when a newer version is available.

## Invariants

- Bot tokens live only in `/switch/nxgallery/telegram-bot.conf` and are never compiled, logged, cached, or packaged.
- TLS peer and host verification remain enabled with the reviewed CA bundle.
- Release updates require the expected public-repository URL, exact NRO asset name, GitHub-provided SHA-256 digest, and a structurally valid NRO before installation.
- Album access is read-only.
- TDLib enters the production boundary only if account-wide chat/history features become required.

## Non-goals for the current slice

- Editing or deleting captures.
- In-app MP4 playback or trimming.
- Telegram user-account login or complete historical chat enumeration.
