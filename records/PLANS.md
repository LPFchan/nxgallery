# NX Gallery Plans

## Approved directions

### Device smoke test

- Outcome: validate the complete grid-to-chat-to-delivery flow on a Switch.
- Why accepted: host tests and an NRO link cannot prove Horizon filesystem, rendering, or network behavior.
- Preconditions: install the generated SD package and real credential file.
- Related ids: DEC-20260717-001

### Refine from device evidence

- Outcome: correct layout, memory, decoding, and upload behavior observed during smoke testing.
- Why accepted: the prototype should follow actual device behavior rather than add speculative compatibility layers.
- Preconditions: captured screenshots/logs from the first smoke test.

## Sequencing

### Near term

- Install the production package on Switch hardware, verify photo share, verify video share, and confirm that a discovered chat persists across relaunch.

### Deferred

- MP4 playback and thumbnail extraction, pending proof that sharing is reliable.
- TDLib integration, pending an explicit requirement for user-account chat/history access.
