#!/bin/sh
set -eu

if [ "$#" -lt 1 ] || [ "$#" -gt 2 ]; then
    echo "usage: $0 SWITCH_IP [nxgallery.nro]" >&2
    exit 2
fi

switch_ip=$1
repo_root=$(CDPATH= cd "$(dirname "$0")/.." && pwd)
nro=${2:-$repo_root/artifacts/nxgallery.nro}
probe_config=${PROBE_CONFIG:-$repo_root/.secrets/telegram-bot.conf}
probe_port=${PROBE_CONFIG_PORT:-28772}
run_mode=${NXGALLERY_RUN_MODE:-probe}
send_media=${NXGALLERY_PROBE_SEND_MEDIA:-0}
nxlink_bin=${NXLINK_BIN:-}
if [ -z "$nxlink_bin" ]; then
    nxlink_bin=$(command -v nxlink || true)
fi
if [ -z "$nxlink_bin" ] && [ -x /opt/devkitpro/tools/bin/nxlink ]; then
    nxlink_bin=/opt/devkitpro/tools/bin/nxlink
fi

[ -x "$nxlink_bin" ] || { echo "nxlink not found; set NXLINK_BIN" >&2; exit 1; }
[ -s "$nro" ] || { echo "NRO not found: $nro" >&2; exit 1; }
[ -s "$probe_config" ] || { echo "probe config not found; set PROBE_CONFIG" >&2; exit 1; }
case "$run_mode" in
    probe|interactive) ;;
    *) echo "NXGALLERY_RUN_MODE must be probe or interactive" >&2; exit 2 ;;
esac
case "$send_media" in
    0|1) ;;
    *) echo "NXGALLERY_PROBE_SEND_MEDIA must be 0 or 1" >&2; exit 2 ;;
esac

if command -v ipconfig >/dev/null 2>&1 && command -v route >/dev/null 2>&1; then
    interface=$(route -n get "$switch_ip" | awk '/interface:/{print $2; exit}')
    host_ip=$(ipconfig getifaddr "$interface")
elif command -v ip >/dev/null 2>&1; then
    host_ip=$(ip route get "$switch_ip" | awk '{for (i=1; i<=NF; i++) if ($i=="src") {print $(i+1); exit}}')
else
    host_ip=${PROBE_HOST_IP:-}
fi
[ -n "${host_ip:-}" ] || { echo "could not determine probe host IP; set PROBE_HOST_IP" >&2; exit 1; }

probe_tls=$(mktemp -d "${TMPDIR:-/tmp}/nxgallery-probe-tls.XXXXXX")
server_pid=
tee_pid=
cleanup() {
    if [ -n "$server_pid" ]; then kill "$server_pid" 2>/dev/null || true; fi
    if [ -n "$tee_pid" ]; then kill "$tee_pid" 2>/dev/null || true; fi
    rm -rf "$probe_tls"
}
trap cleanup EXIT HUP INT TERM

openssl req -x509 -newkey rsa:2048 -nodes -days 1 -subj /CN=nxgallery-probe \
    -keyout "$probe_tls/key.pem" -out "$probe_tls/cert.pem" >/dev/null 2>&1
pin_hex=$(openssl x509 -in "$probe_tls/cert.pem" -pubkey -noout | \
    openssl pkey -pubin -outform DER | openssl dgst -sha256 -binary | \
    od -An -tx1 | tr -d '[:space:]')
python3 "$repo_root/scripts/probe-config-server.py" \
    --config "$probe_config" --cert "$probe_tls/cert.pem" \
    --key "$probe_tls/key.pem" --port "$probe_port" &
server_pid=$!

launch_args="--probe-config-url=https://$host_ip:$probe_port/config --probe-config-pin-hex=$pin_hex"
if [ "$run_mode" = probe ]; then launch_args="--probe $launch_args"; fi
if [ "$run_mode" = probe ] && [ "$send_media" = 1 ]; then
    launch_args="--probe-send-media $launch_args"
fi

status=0
probe_log=${PROBE_LOG:-$repo_root/artifacts/hardware-probe-last.log}
mkdir -p "$(dirname "$probe_log")"
: > "$probe_log"
chmod 600 "$probe_log"
probe_pipe="$probe_tls/nxlink.pipe"
mkfifo "$probe_pipe"
tee "$probe_log" < "$probe_pipe" &
tee_pid=$!
"$nxlink_bin" --address "$switch_ip" --retries 5 --server \
    --args "$launch_args" "$nro" > "$probe_pipe" 2>&1 || status=$?
wait "$tee_pid" || status=1
tee_pid=
if ! grep -Fq 'NXGALLERY_DIAGNOSTIC event=startup' "$probe_log"; then
    echo 'nxlink did not receive an NX Gallery startup diagnostic; verify hbmenu NetLoader is active' | tee -a "$probe_log" >&2
    status=1
fi
if [ "$run_mode" = probe ]; then
    if [ "$send_media" = 1 ]; then
        expected='NXGALLERY_PROBE_RESULT result=pass sd=pass playback=pass network=pass photo=pass video=pass'
    else
        expected='NXGALLERY_PROBE_RESULT result=pass sd=pass playback=pass chats=pass photo=skip video=skip'
    fi
    if ! grep -Fqx "$expected" "$probe_log"; then
        echo "hardware probe did not complete every required phase" >&2
        status=1
    fi
fi
kill "$server_pid" 2>/dev/null || true
wait "$server_pid" 2>/dev/null || true
server_pid=
exit "$status"
