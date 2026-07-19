# NX Gallery Plans

## Approved directions

### Refine from device evidence

- Outcome: correct layout, memory, decoding, and upload behavior observed during smoke testing.
- Why accepted: the prototype should follow actual device behavior rather than add speculative compatibility layers.
- Preconditions: met by the passing nxlink hardware probe and persisted crash diagnostics.

## Sequencing

### Near term

- Run one non-sending nxlink probe to gate the corrected MP4 file I/O and pause/resume automatically, then exercise progressive startup, QR onboarding, multi-select, the cached picker, and transfer progress with captured diagnostics. Validate one mixed-media Telegram album as an explicit one-shot delivery operation; media-delivery probes must not retry automatically.

### Deferred

- Audio decoding/output for MP4 playback.
- User-account TDLib integration, pending explicit acceptance of personal-account authentication; bot-authorized TDLib remains update-only.
