# NX Gallery Plans

## Approved directions

### Refine from device evidence

- Outcome: correct layout, memory, decoding, and upload behavior observed during smoke testing.
- Why accepted: the prototype should follow actual device behavior rather than add speculative compatibility layers.
- Preconditions: met by the passing nxlink hardware probe and persisted crash diagnostics.

## Sequencing

### Near term

- Run one non-sending nxlink probe to gate MP4 audio, pause/resume, and left-stick seeking on hardware, then exercise progressive startup, QR onboarding, large multi-select, the cached picker, and whole-selection transfer progress with captured diagnostics. Validate one selection larger than ten as an explicit one-shot Telegram delivery operation; media-delivery probes must not retry automatically.

### Deferred

- User-account TDLib integration, pending explicit acceptance of personal-account authentication; bot-authorized TDLib remains update-only.
