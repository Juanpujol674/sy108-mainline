#!/bin/sh
# Compile the maintained cellular service, typed clients.
set -eu
HERE=$(CDPATH= cd -- "$(dirname -- "$0")" && pwd)
OUT=${1:?output directory}
JSON_LIBRARY=${2:?OpenWrt libjson-c library or linker argument}
CC=${CC:-musl-gcc}
CFLAGS=${CFLAGS:--Os -Wall -Wextra -Werror -Wno-misleading-indentation}
JSON_INCLUDE=${JSON_INCLUDE:-/usr/include}
SOURCE=${UDX_CELLULAR_SOURCE:-$HERE/src/native}
mkdir -p "$OUT/.cellular-objects"
OBJECTS=$OUT/.cellular-objects
for name in common digest format sms_pdu owner transport data receipts sms_history apn identity operators radio advanced modes info debug telemetry controller service client quiesce; do
    "$CC" ${CPPFLAGS:-} $CFLAGS -std=c11 -I"$JSON_INCLUDE" -I"$SOURCE" -c "$SOURCE/$name.c" -o "$OBJECTS/$name.o"
done
service_objects=''
for name in common digest format sms_pdu owner transport data receipts sms_history apn identity operators radio advanced modes info debug telemetry controller service; do
    service_objects="$service_objects $OBJECTS/$name.o"
done
"$CC" $CFLAGS $service_objects ${LDFLAGS:-} "$JSON_LIBRARY" -lcrypto -pthread -o "$OUT/udx-cellulard"
for spec in 'client:udx-cellular-control' 'quiesce:udx-system-quiesce'; do
    source=${spec%%:*}
    name=${spec#*:}
    extra=''
    [ "$source" != quiesce ] || extra="$OBJECTS/owner.o"
    "$CC" $CFLAGS "$OBJECTS/$source.o" "$OBJECTS/common.o" $extra ${LDFLAGS:-} "$JSON_LIBRARY" -lcrypto -pthread -o "$OUT/$name"
done
