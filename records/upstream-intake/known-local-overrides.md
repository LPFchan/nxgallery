# Known Local Overrides

Use this register to record intentional downstream divergences so they do not have to be rediscovered from scratch every review.

Only record stable, intentional divergences here.
Do not use this file for temporary experiments or unreviewed preferences.

## Entry Template

- Area:
- Local surface:
- Upstream surface:
- Why the fork diverged:
- Collision rule to apply during intake:
- Revisit trigger:
- Related decision record:

## Current Entries

- Area: Telegram direction
- Local surface: `source/telegram_bot.cpp` and DEC-20260717-001
- Upstream surface: NX Torrent's direct Bot API receive queue and TDLib Horizon experiment
- Why the fork diverged: NX Gallery sends captures and needs a destination picker; NX Torrent receives torrent documents from one allowed chat.
- Collision rule to apply during intake: reuse transport hardening and Horizon compatibility, but keep picker/send policy local to NX Gallery.
- Revisit trigger: NX Torrent changes CA, curl, Bot API error handling, or persisted-update semantics.
- Related decision record: DEC-20260717-001
