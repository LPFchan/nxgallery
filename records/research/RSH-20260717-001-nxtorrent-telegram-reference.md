# RSH-20260717-001: NX Torrent And Telegram Reference Boundary

Opened: 2026-07-17 18-14-57 KST
Recorded by agent: root

## Question

Which NX Torrent and upstream interfaces should NX Gallery reuse, and where is TDLib actually required?

## Findings

- The live reference is the sibling NX Torrent repository at `../nxtorrent/app/nxtorrent`.
- Reused patterns: Plutonium renderer initialization, curl/json-c Bot API requests, OpenSSL random seeding, fixed diagnostics, bounded responses, CA preflight, and Horizon POSIX compatibility.
- The reviewed CA bundle is tracked at `payload/switch/nxgallery/openssl/cert.pem` with SHA-256 `779d1ee15982d65903b60ad9b5a4502630519dd6e6b6f58cf7793c349302bbc0`.
- Telegram Bot API pending updates last no longer than 24 hours and supply chat metadata for observed messages or membership changes.
- The Bot API currently documents 10 MB photo and 50 MB video upload limits; these are enforced before upload.
- No Bot API method provides a complete membership/chat list. This is the decisive boundary: configured/observed picker uses Bot API; account-wide picker/history uses TDLib with a user identity.
- The supplied TDLib smoke configuration contains a usable bot token and seed chat, but `api_id` and `api_hash` are not required by NX Gallery and are not copied into its configuration.

## Evidence

- Telegram Bot API: https://core.telegram.org/bots/api
- Plutonium upstream: https://github.com/XorTroll/Plutonium
- libnx capture/album surface: https://github.com/switchbrew/libnx
- Local NX Torrent direct Bot API decision: `DEC-20260717-002-direct-bot-api-queue.md` in the reference repo.

## Follow-up

- Validate actual Switch album paths and SDL image decoding.
- Verify the bot receives one screenshot and one 30-second MP4.
- Revisit TDLib only if configured/observed destinations fail the desired UX.
