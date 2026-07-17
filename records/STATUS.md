# NX Gallery Status

## Snapshot

- Last updated: 2026-07-17
- Overall posture: `active`
- Current focus: physical Switch delivery smoke test and UX refinement
- Highest-priority blocker: no physical Switch validation yet; chat-cache persistence now needs confirmation on Horizon
- Next operator decision needed: validate photo and video delivery on hardware, then decide whether to add video playback
- Related decisions: DEC-20260717-001

## Current state

The repo-template v1.1.4 operating model is installed. The portable album, navigation, and configuration core passes host tests. Production and emulator-automation Plutonium/libnx builds link successfully on `yeowoolmac`. Telegram upload is asynchronous and the runtime configuration is kept outside Git.

Ryujinx 1.3.3 now passes the photo-delivery pre-hardware gate: the production NRO
loads its embedded ASET/NACP, renders two virtual-SD fixtures at about 60 FPS,
discovers the operator's private bot chat, and completes a real `sendPhoto`
through the NX Gallery picker. The result screen confirms delivery. The picker
also loads the Switch Korean shared font, preserves UTF-8 boundaries while
clipping, and renders its rows, buttons, and controller hints without overlap.
Chat-cache writes now use an explicit `sdmc:/` path and report failures instead
of silently claiming success. The packaged example no longer supplies a fake
destination. Albums exceeding 5,000 supported files are fully ordered before
the newest 5,000 are retained. Ryujinx confirms that refresh rewrites the cache
and that both the discovered private chat and its Korean title survive a cold
app relaunch.

## Active tracks

### Device validation

- Status: `Ryujinx photo delivery passed; hardware and video pending`
- Goal: verify album visibility, thumbnail decoding, controller flow, chat refresh, and real photo/video delivery on Horizon.
- Exit criteria: one photo and one 30-second capture arrive from physical Switch hardware in an explicitly selected chat.
- Risk: Bot API upload limits, Horizon networking, or physical SD paths may differ from Ryujinx.

### Product completeness

- Status: `not started`
- Goal: decide whether playback, delete/edit actions, and richer thumbnail generation belong in the next slice.
- Dependency: successful physical-device share validation.
