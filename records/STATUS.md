# NX Gallery Status

## Snapshot

- Last updated: 2026-07-17
- Overall posture: `active`
- Current focus: physical Switch validation of playback controls, cached chat UX, transfer progress, and hbmenu return
- Highest-priority blocker: none for SD enumeration, networking, or Telegram photo/video delivery
- Next operator decision needed: whether user-account TDLib login is acceptable for exhaustive Telegram chat enumeration
- Related decisions: DEC-20260717-001

## Current state

The repo-template v1.1.4 operating model is installed. The portable album, navigation, and configuration core passes host tests. Production and emulator-automation Plutonium/libnx builds link successfully on `yeowoolmac`. Telegram upload is asynchronous, reports libcurl transfer progress in the share sheet, and keeps runtime configuration outside Git. Chat destinations load from memory immediately while a single launch-time Bot API refresh updates the cache in the background.

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

Hardware validation is now self-driving: nxlink `--probe` diagnostics cover SD
enumeration, destination refresh, photo delivery, and MP4 delivery, ending in a
machine-readable summary without requiring the operator to inspect or relay UI
state. The probe also queries Horizon's NAND and SD Album Accessor stores and
persists native exception context for post-crash diagnosis.

The first hardware inventory proved that Horizon exposes 107 SD captures and
one NAND capture through `caps:a`, while raw `sdmc:/Nintendo/Album` traversal
returns no media in the homebrew launch context. The Switch runtime now uses
Album Accessor enumeration and lazily materializes selected photos or MP4
streams for rendering and Bot API upload.

The physical Switch probe now passes end to end. It enumerated 68 photos and 40
videos from 107 SD plus one NAND Album Accessor entry, authenticated its ephemeral
credential channel, resolved two saved destinations, and delivered one real
photo plus one real MP4 through Telegram. The harness treats any missing phase
as a nonzero failure and the operator independently observed both deliveries.
Telegram confirmed the uploaded MP4 as 1280x720 and the operator confirmed its
16:9 aspect ratio. Album Accessor video thumbnails and FFmpeg-backed video-only
playback are built. A non-sending hardware probe isolated playback startup to
FFmpeg interpreting `sdmc:` as an unsupported URL protocol; the player now uses
the file protocol explicitly, the Album Accessor materializer honors short
reads, and a fresh cache generation prevents reuse of older malformed files.
That correction is built but awaits the next hbmenu NetLoader window for its
one-shot pause/resume verification.

## Active tracks

### Device validation

- Status: `physical Switch photo and video delivery passed`
- Goal: verify album visibility, thumbnail decoding, controller flow, chat refresh, and real photo/video delivery on Horizon.
- Exit criteria: met; one photo and one capture arrived from physical Switch hardware in a saved destination.
- Risk: interactive hardware UI behavior and chat-cache persistence remain less automated than backend delivery.

### Product completeness

- Status: `playback and transfer UX implemented; hardware validation pending`
- Goal: validate playback pause/resume, transfer progress, cached chat opening, and hbmenu return; audio remains a separate playback gap.
- Dependency: successful physical-device share validation.
