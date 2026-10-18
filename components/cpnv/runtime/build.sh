#!/bin/bash
# SPDX-License-Identifier: GPL-2.0-only
set -euo pipefail

ROOT=$(cd -- "$(dirname -- "$0")" && pwd)
OUT=${1:?usage: build.sh OUTPUT_DIRECTORY}
mkdir -p "$OUT"
OUT=$(cd "$OUT" && pwd)
BASE="$ROOT/components"
CC=${CC:-musl-gcc}

P=supervisor
OLD=nv-supervisor
NV=nv-backend/nv-continuous
UBI=nv-backend/ubi-backend
CODEC=nv-codec
MON=session
SHA=common
MC=modem
MCO=modem-core
RN=notify
HALO=halo

mkdir -p "$OUT/generated"
if [ "${CPNV_TARGET_SDK:-0}" != 1 ]; then
	mkdir -p "$OUT/uapi"
	for d in linux mtd asm-generic; do
		ln -sfn "/usr/include/$d" "$OUT/uapi/$d"
	done
	ln -sfn "/usr/include/$(gcc -print-multiarch)/asm" "$OUT/uapi/asm"
fi

flags=(
	-std=c11
	-Os
	-g0
	-Wall
	-Wextra
	-Werror
	-Wno-misleading-indentation
	-ffunction-sections
	-fdata-sections
)
inc=(-I"$OUT/generated")
if [ "${CPNV_TARGET_SDK:-0}" != 1 ]; then
	inc+=(-idirafter "$OUT/uapi")
fi
if [ -n "${CFLAGS:-}" ]; then
	read -r -a extra_flags <<< "$CFLAGS"
	flags+=("${extra_flags[@]}")
fi

inc+=(-I"$ROOT/../storage")
VENDOR=("$ROOT/../storage/nv_vendor.c")
NV_BACKEND="$ROOT/../storage/nv_vendor_backend.c"
NV_COMPRESSION=("$ROOT/../storage/third_party/lz4/lz4.c")

for d in \
	"$P/include" \
	"$OLD/include" \
	"$MC/include" \
	"$MCO/include" \
	"$SHA/components/nv/include" \
	"$NV/include" \
	"$UBI/include" \
	"$CODEC/include" \
	"$MON/include" \
	"$SHA/include" \
	"$RN/include" \
	"$RN/integration" \
	"$HALO/include"; do
	inc+=(-I"$BASE/$d")
done

sources() {
	local component=$1
	shift
	for n in "$@"; do
		printf '%s\n' "$BASE/$component/src/$n.c"
	done
}

build() {
	local name=$1
	shift
	"$CC" "${flags[@]}" "${inc[@]}" -static -Wl,--gc-sections,-z,defs "$@" -o "$OUT/$name"
	printf 'Built %s\n' "$name"
}

mapfile -t controller < <(
	sources "$MC" modem_main modem_profile modem_linux modem_reload
	sources "$MCO" modem_halo halo_peer_gate modem_cproc modem_names modem_image modem_cmdline
	sources "$SHA" sha256
	sources "$HALO" halo_auth halo_material halo_feature halo_ap_state
	sources "$CODEC" nv_codec
	sources "$MON" session_monitor
)
build udx-modem-control-warm "${controller[@]}" "${VENDOR[@]}"

mapfile -t core < <(
	sources "$NV" nv_schedule nv_epoch
	printf '%s\n' "$NV_BACKEND"
	if [ ${#NV_COMPRESSION[@]} -gt 0 ]; then
		printf '%s\n' "${NV_COMPRESSION[@]}"
	fi
	sources "$UBI" nv_ubi_linux
	sources "$CODEC" nv_codec
	sources "$MON" session_monitor
	sources "$P" wr_quiesce nv_wire
)
build udx-nv-continuous-v4 "$BASE/$P/src/nv_continuous.c" "${core[@]}" "${VENDOR[@]}"

mapfile -t broker < <(
	sources "$OLD" nvs_report
	sources "$P" cpnv_fd_plan
)
build udx-nv-broker-v4 "$BASE/$P/src/nv_broker.c" "${core[@]}" "${broker[@]}" "${VENDOR[@]}"

mapfile -t notify < <(
	sources "$RN" main daemon session
	for n in calendar protocol stream timesync linux; do
		printf '%s\n' "$BASE/$RN/core/$n.c"
	done
)
build udx-refnotifyd "${notify[@]}"

mapfile -t parent < <(
	sources "$P" wr_main wr_control wr_platform wr_parent wr_controller wr_state wr_postload cpnv_linux cpnv_observer cpnv_fd_plan
	sources "$P" wr_notify rt_parent
	sources "$RN" devices session
	for n in linux calendar protocol; do
		printf '%s\n' "$BASE/$RN/core/$n.c"
	done
	sources "$P" wr_factory wr_policy wr_snapshot
	sources "$MCO" modem_image modem_cmdline
	sources "$MC" modem_profile
	sources "$SHA" sha256
	sources "$CODEC" nv_codec
	sources "$P" wr_nv nvs_launch wr_quiesce wr_roles wr_devices
	sources "$OLD" nvs_report
	sources "$NV" nv_epoch
	sources "$MON" session_monitor
)
build udx-cpnv-parent "${parent[@]}" "${VENDOR[@]}"
