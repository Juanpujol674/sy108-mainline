#!/bin/sh
# OpenWrt owns image transport, metadata integrity, staging and reboot.
REQUIRE_IMAGE_METADATA=1
REQUIRE_IMAGE_SIGNATURE=0
CI_UBIPART=ubipac
CI_KERNPART=boot
RAMFS_COPY_BIN="$RAMFS_COPY_BIN /usr/libexec/udx-boot-state ubiupdatevol ubiblock"

sy108_volume() {
	local ubi volume
	ubi=$(nand_find_ubi "$CI_UBIPART") || return 1
	volume=$(nand_find_volume "$ubi" "$1") || return 1
	[ -n "$volume" ] || return 1
	[ "$(cat /sys/class/ubi/$volume/type)" = dynamic ] || return 1
	[ "$(cat /sys/class/ubi/$volume/reserved_ebs)" = "$2" ] || return 1
	[ "$(cat /sys/class/ubi/$volume/usable_eb_size)" = 126976 ] || return 1
	printf '%s\n' "$volume"
}

platform_check_image() (
	# fwtool validates the whole-image CRC while reading device metadata.
	[ "${FWTOOL_DEVICE_MATCH:-1}" = 0 ] || return 74
	set -o pipefail
	local directory root_bytes
	directory=$(mktemp -d /tmp/sy108-image-check.XXXXXX) || return 74
	trap 'rm -rf "$directory"' EXIT
	[ "$(tar -tf "$1" | awk '$0=="sysupgrade-sy108/kernel" {k++} $0=="sysupgrade-sy108/root" {r++} END {print k ":" r}')" = 1:1 ] || return 74
	tar -xOf "$1" sysupgrade-sy108/kernel > "$directory/kernel" || return 74
	root_bytes=$(tar -xOf "$1" sysupgrade-sy108/root | wc -c) || return 74
	tar -xOf "$1" sysupgrade-sy108/root | {
		head -c 96 > "$directory/root-header"
		cat >/dev/null
	} || return 74
	/usr/libexec/udx-image-check "$directory/kernel" "$directory/root-header" "$root_bytes" || return 74
	sy108_volume boot 133 >/dev/null && sy108_volume rootfs 397 >/dev/null && sy108_volume rootfs_data 426 >/dev/null || return 74
)

# This early hook runs only for a real upgrade, before procd terminates CP/NV.
platform_prepare_upgrade() {
	/usr/libexec/udx-prepare-upgrade
}

platform_quiesce_usb() {
	local gadget=/sys/kernel/config/usb_gadget/udx-usb/UDC
	local udc=/sys/class/udc/29100000.dwc3
	local pam=/sys/bus/platform/devices/2b300000.pamu3/offload_state
	local bound state
	# RAM stage2 has already taken over; losing the management link here is
	# expected. A forced reboot with active SS endpoints can poison the next
	# boot's transfers even though the new device enumerates successfully.
	if [ -f "$gadget" ]; then
		read -r bound < "$gadget" || return 1
		case "$bound" in
			29100000.dwc3) printf '\n' > "$gadget" || return 1 ;;
			'') ;;
			*)
				echo 'Unexpected USB gadget controller' >&2
				return 1
				;;
		esac
	fi
	if [ -f "$udc/state" ]; then
		read -r state < "$udc/state" || return 1
		[ "$state" = 'not attached' ] || {
			echo 'USB controller did not detach' >&2
			return 1
		}
	fi
	if [ -f "$pam" ]; then
		read -r state < "$pam" || return 1
		case "$state" in
			'clock=0 prepared=0 claimed=0 running=0 connected=0 requested=0 '*) ;;
			*)
				echo 'USB accelerator still owns DMA resources' >&2
				return 1
				;;
		esac
	fi
}

platform_do_upgrade() {
	local image="$1" boot root data kernel_bytes root_bytes
	set -o pipefail
	boot=$(sy108_volume boot 133) || exit 1
	root=$(sy108_volume rootfs 397) || exit 1
	data=$(sy108_volume rootfs_data 426) || exit 1
	platform_quiesce_usb || exit 1
	if [ -e "/sys/class/block/ubiblock${root#ubi}" ]; then
		ubiblock -r "/dev/$root" || exit 1
	fi
	[ ! -e "/sys/class/block/ubiblock${root#ubi}" ] || exit 1
	kernel_bytes=$(tar -xOf "$image" sysupgrade-sy108/kernel | wc -c) || exit 1
	root_bytes=$(tar -xOf "$image" sysupgrade-sy108/root | wc -c) || exit 1
	tar -xOf "$image" sysupgrade-sy108/kernel | ubiupdatevol "/dev/$boot" -s "$kernel_bytes" - || exit 1
	tar -xOf "$image" sysupgrade-sy108/root | ubiupdatevol "/dev/$root" -s "$root_bytes" - || exit 1
	ubiupdatevol "/dev/$data" -t || exit 1
	if [ -z "$UPGRADE_BACKUP" ]; then
		/usr/libexec/udx-boot-state trial || exit 1
	fi
}

platform_copy_config() {
	# Standard OpenWrt UBIFS backup placement; fstools restores it on next boot.
	nand_restore_config "$UPGRADE_BACKUP" || exit 1
	/usr/libexec/udx-boot-state trial || exit 1
}

if [ -f /etc/udx-recovery-root ]; then
	platform_restore_backup() {
		/usr/libexec/udx-recovery-config restore "$CONF_RESTORE"
	}
	platform_create_backup() {
		/usr/libexec/udx-recovery-config backup "$1" "${SKIP_UNCHANGED:-0}" "${SAVE_INSTALLED_PKGS:-0}" "${SAVE_OVERLAY_PATH:-}"
	}
fi
