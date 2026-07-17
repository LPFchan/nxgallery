#!/bin/sh
set -eu

repo_root=$(CDPATH= cd "$(dirname "$0")/.." && pwd)
example="$repo_root/telegram-bot.conf.example"

[ -s "$repo_root/payload/switch/nxgallery/openssl/cert.pem" ]
[ -s "$repo_root/icon.jpg" ]
grep -Fq 'export NROFLAGS += --icon=$(APP_ICON) --nacp=$(CURDIR)/$(TARGET).nacp' "$repo_root/Makefile"
grep -Fq '__nx_heap_size = 256ULL * 1024ULL * 1024ULL' "$repo_root/source/main.cpp"
grep -Fq 'options.AddDefaultSharedFont(PlSharedFontType_KO);' "$repo_root/source/main.cpp"
grep -Fq 'scan_horizon_album' "$repo_root/source/main.cpp"
grep -Fq 'NXGALLERY_PROBE_RESULT result=pass sd=pass network=pass photo=pass video=pass' \
    "$repo_root/scripts/run-hardware-probe.sh"
grep -Fq 'sdmc:/switch/nxgallery/crash.log' "$repo_root/source/crash_diagnostics.cpp"
[ -x "$repo_root/scripts/run-hardware-probe.sh" ]
sh -n "$repo_root/scripts/run-hardware-probe.sh"
grep -q '^discover_chats=true$' "$example"
! grep -q '^chat=' "$example"
grep -Fq 'sdmc:/switch/nxgallery/telegram-state.json' "$repo_root/source/telegram_bot.cpp"
! grep -Fq '(void)save_state' "$repo_root/source/telegram_bot.cpp"
! grep -Eq '^bot_token=[0-9]{5,}:[A-Za-z0-9_-]{20,}$' "$example"
! rg -l -uu '^[[:space:]]*(api_hash|api_id)[[:space:]]*=' "$repo_root" \
    -g '!.secrets/**' -g '!records/**' >/dev/null
printf 'nxgallery release contract passed\n'
