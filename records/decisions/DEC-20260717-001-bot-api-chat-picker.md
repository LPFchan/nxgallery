# DEC-20260717-001: Use Bot API Discovery For The Share Picker

Opened: 2026-07-17 18-14-57 KST
Recorded by agent: root

## Metadata

- Status: accepted
- Deciders: operator, orchestrator
- Related ids: RSH-20260717-001

## Decision

Use the Telegram HTTPS Bot API for NX Gallery. Build the share picker from configured chats, credential-free persisted chats, and chats observed through `getUpdates`. Upload photos with `sendPhoto` and videos with `sendVideo` only after explicit operator selection.

Do not ship TDLib in the current dependency boundary. Reconsider TDLib only for user-account login, account-wide chat enumeration/search, or historical chat access.

## Context

The Bot API has no method that enumerates every chat a bot belongs to. It can expose chat metadata through incoming updates and resolve known identifiers. Telegram keeps pending updates for no longer than 24 hours. NX Torrent already demonstrates direct Bot API HTTPS on Horizon.

The operator accepts that one Switch app runs at a time and accepts sequential update-offset interaction between NX Gallery and NX Torrent during this prototype.

## Options considered

### Bot API with configured and observed chats

- Smaller and already proven on Horizon.
- Directly supports the required picker and uploads.
- Cannot provide a complete historical account chat list.

### TDLib bot identity

- Reuses the existing Horizon port.
- Adds MTProto, database, and startup surface without granting bot history enumeration.

### TDLib user identity

- Provides complete account chat/history behavior.
- Introduces phone/login/session UX and materially changes the trust boundary.

## Consequences

- Chats appear when configured or observed in pending updates.
- Chat metadata and offsets persist without the bot token.
- The real token remains SD-only configuration.
- A later full-account picker is an explicit TDLib product change, not a hidden extension of this transport.
