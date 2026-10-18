#!/bin/sh
. /lib/functions.sh
. /lib/netifd/netifd-proto.sh
init_proto "$@"
proto_udx_init_config() {
	no_device=1
	available=1
	proto_config_add_string device
	proto_config_add_int metric
}
proto_udx_setup() {
	local interface="$1" device metric
	json_get_vars device metric
	[ "$device" = sipa_eth0 ] || { proto_notify_error "$interface" INVALID_DEVICE; proto_block_restart "$interface"; return 1; }
	proto_run_command "$interface" /usr/libexec/udx-netifd "$interface" "$device" "${metric:-50}"
}
proto_udx_teardown() { proto_kill_command "$1"; }
add_protocol udx
