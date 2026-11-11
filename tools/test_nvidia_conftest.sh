#!/usr/bin/env bash
# SPDX-FileCopyrightText: Copyright (c) 2026 The libmc authors
# SPDX-License-Identifier: MIT
#
# Host-only test of tools/nvidia_conftest.sh against synthetic driver trees,
# so it needs neither a second driver release nor network access:
#   - a probe reports 1 when the field exists and 0 when it does not;
#   - the release string comes from the tree's version.mk;
#   - an unchanged tree leaves the header (and its mtime) alone;
#   - a broken include path or a missing version.mk is a hard error.
#
# Usage: test_nvidia_conftest.sh CC
set -euo pipefail
cc=${1:-cc}
here=$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")" && pwd)
conftest=$here/nvidia_conftest.sh
work=$(mktemp -d)
trap 'rm -rf "$work"' EXIT
failures=0
pass() { echo "PASS: $1"; }
fail() { echo "FAIL: $1" >&2; failures=$((failures + 1)); }

# make_tree DIR VERSION WITH_HANDLE_VASPACE(0|1)
make_tree() {
    local d=$1
    mkdir -p "$d/inc/alloc"
    printf 'NVIDIA_VERSION = %s\n' "$2" > "$d/version.mk"
    printf 'typedef unsigned int NvU32;\ntypedef NvU32 NvHandle;\n' > "$d/inc/nvtypes.h"
    printf '#include "nvtypes.h"\n' > "$d/inc/nvos.h"
    {
        printf 'typedef struct {\n    NvHandle hVASpace;\n'
        [[ $3 == 1 ]] && printf '    NvHandle hHandleVASpace;\n'
        printf '} NV_CHANNEL_ALLOC_PARAMS;\n'
    } > "$d/inc/alloc/alloc_channel.h"
}
value_of() { sed -n "s/^#define $1 //p" "$2"; }

make_tree "$work/old" 595.99.02 0
make_tree "$work/new" 610.43.02 1

bash "$conftest" "$work/old" "$work/old.h" "$cc" -I"$work/old/inc" > /dev/null
bash "$conftest" "$work/new" "$work/new.h" "$cc" -I"$work/new/inc" > /dev/null
[[ $(value_of MC_NV_HAVE_CHANNEL_HANDLE_VASPACE "$work/old.h") == 0 ]] \
    && pass "probe reports an absent field as 0" || fail "absent field not reported as 0"
[[ $(value_of MC_NV_HAVE_CHANNEL_HANDLE_VASPACE "$work/new.h") == 1 ]] \
    && pass "probe reports a present field as 1" || fail "present field not reported as 1"
[[ $(value_of MC_NVIDIA_DRIVER_VERSION "$work/new.h") == '"610.43.02"' ]] \
    && pass "release string comes from version.mk" || fail "wrong release string"

touch -d '2000-01-01' "$work/new.h"
bash "$conftest" "$work/new" "$work/new.h" "$cc" -I"$work/new/inc" > /dev/null
[[ $(stat -c %Y "$work/new.h") == $(date -d '2000-01-01' +%s) ]] \
    && pass "an unchanged tree leaves the header untouched" || fail "header rewritten without a change"

if bash "$conftest" "$work/new" "$work/bad.h" "$cc" -I"$work/missing" 2> /dev/null; then
    fail "a broken include path was accepted"
else
    pass "a broken include path is a hard error"
fi
rm "$work/old/version.mk"
if bash "$conftest" "$work/old" "$work/bad.h" "$cc" -I"$work/old/inc" 2> /dev/null; then
    fail "a tree without version.mk was accepted"
else
    pass "a tree without version.mk is a hard error"
fi

exit $((failures > 0))
