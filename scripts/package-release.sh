#!/bin/sh
set -eu

if [ "$#" -ne 2 ]; then
    echo "usage: $0 /path/to/nxgallery.nro /path/to/output/nxgallery.nro" >&2
    exit 2
fi

input_nro=$1
output_parent=$(CDPATH= cd "$(dirname "$2")" && pwd)
output_nro="$output_parent/$(basename "$2")"

[ -f "$input_nro" ] || { echo "NRO not found: $input_nro" >&2; exit 1; }

read_u32_le() {
    LC_ALL=C od -An -tu4 -N 4 -j "$2" "$1" | tr -d '[:space:]'
}

read_magic() {
    LC_ALL=C od -An -tc -N "$3" -j "$2" "$1" | tr -d '[:space:]'
}

read_hex() {
    LC_ALL=C od -An -tx1 -N "$3" -j "$2" "$1" | tr -d '[:space:]'
}

validate_nro_assets() {
    nro=$1
    file_size=$(LC_ALL=C wc -c < "$nro" | tr -d '[:space:]')
    [ "$file_size" -ge 184 ] || return 1
    [ "$(read_magic "$nro" 16 4)" = NRO0 ] || return 1
    nro_size=$(read_u32_le "$nro" 24)
    [ -n "$nro_size" ] || return 1
    [ "$nro_size" -ge 128 ] || return 1
    [ "$nro_size" -le $((file_size - 56)) ] || return 1
    [ "$(read_magic "$nro" "$nro_size" 4)" = ASET ] || return 1

    icon_offset=$(read_u32_le "$nro" $((nro_size + 8)))
    icon_size=$(read_u32_le "$nro" $((nro_size + 16)))
    icon_offset_high=$(read_u32_le "$nro" $((nro_size + 12)))
    icon_size_high=$(read_u32_le "$nro" $((nro_size + 20)))
    [ "$icon_offset_high" = 0 ] || return 1
    [ "$icon_size_high" = 0 ] || return 1
    [ "$icon_size" -gt 0 ] || return 1
    icon_start=$((nro_size + icon_offset))
    [ "$icon_start" -ge $((nro_size + 56)) ] || return 1
    [ "$icon_start" -le $((file_size - icon_size)) ] || return 1
    [ "$(read_hex "$nro" "$icon_start" 3)" = ffd8ff ] || return 1

    nacp_offset=$(read_u32_le "$nro" $((nro_size + 24)))
    nacp_size=$(read_u32_le "$nro" $((nro_size + 32)))
    nacp_offset_high=$(read_u32_le "$nro" $((nro_size + 28)))
    nacp_size_high=$(read_u32_le "$nro" $((nro_size + 36)))
    [ "$nacp_offset_high" = 0 ] || return 1
    [ "$nacp_size_high" = 0 ] || return 1
    [ "$nacp_size" = 16384 ] || return 1
    nacp_start=$((nro_size + nacp_offset))
    [ "$nacp_start" -ge $((nro_size + 56)) ] || return 1
    [ "$nacp_start" -le $((file_size - nacp_size)) ] || return 1
    [ "$(read_hex "$nro" "$nacp_start" 11)" = 4e582047616c6c65727900 ] || return 1
}

validate_nro_assets "$input_nro" || {
    echo "refusing to package an NRO without a valid icon and NX Gallery NACP" >&2
    exit 1
}

cp "$input_nro" "$output_nro"
chmod 0644 "$output_nro"
printf 'Packaged %s\n' "$output_nro"
