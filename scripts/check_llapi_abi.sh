#!/usr/bin/env bash
# Checks backend/llapi.cpp's copy of the Lustre uapi (ioctl numbers, struct sizes,
# constants) against the installed linux/lustre/lustre_user.h (design 06 §6.5).  The
# backend talks to the kernel client directly, so this is the only place a uapi drift
# would be caught at build time.
#
#   scripts/check_llapi_abi.sh [include-dir-with-linux/lustre]
#
# Exit 0: everything matches, or the header is absent (skipped; LNFS_LLAPI_STRICT=1
# makes that exit 2 instead).  Exit 1: drift.
set -euo pipefail
cd "$(dirname "$0")/.."
INC="${1:-/usr/include}"
HDR=""
for cand in "$INC/linux/lustre/lustre_user.h" "$INC/lustre/lustre_user.h"; do
  [ -f "$cand" ] && HDR="$cand" && break
done
if [ -z "$HDR" ]; then
  echo "check_llapi_abi: no lustre_user.h under $INC (install lustre-client-utils / lustre-dev); skipped"
  [ "${LNFS_LLAPI_STRICT:-0}" = 1 ] && exit 2
  exit 0
fi
TMP="$(mktemp -d)"
trap 'rm -rf "$TMP"' EXIT
cat > "$TMP/check.c" <<CHK
#include <sys/ioctl.h>
#include <stddef.h>
#include "$HDR"
#include <assert.h>
#define CHECK(c) _Static_assert((c), #c)
/* struct layouts the backend mirrors (llapi.cpp) */
CHECK(sizeof(struct lu_fid) == 16);
CHECK(offsetof(struct lu_fid, f_seq) == 0 && offsetof(struct lu_fid, f_oid) == 8 &&
      offsetof(struct lu_fid, f_ver) == 12);
CHECK(sizeof(struct hsm_user_state) == 32);
CHECK(offsetof(struct hsm_user_state, hus_states) == 0);
CHECK(offsetof(struct hsm_user_state, hus_in_progress_state) == 8);
CHECK(offsetof(struct hsm_user_state, hus_in_progress_action) == 12);
CHECK(sizeof(struct hsm_request) == 24);
CHECK(offsetof(struct hsm_request, hr_action) == 0);
CHECK(offsetof(struct hsm_request, hr_itemcount) == 16);
CHECK(sizeof(struct hsm_user_item) == 32);
CHECK(offsetof(struct hsm_user_item, hui_extent) == 16);
CHECK(sizeof(struct hsm_user_request) == 24);
CHECK(offsetof(struct lov_user_md_v1, lmm_stripe_size) == 24);
CHECK(sizeof(struct lov_user_md_v1) == 32);
/* ioctl numbers and constants */
CHECK(LL_IOC_PATH2FID == _IOR('f', 173, long));
CHECK(LL_IOC_HSM_STATE_GET == _IOR('f', 211, struct hsm_user_state));
CHECK(LL_IOC_HSM_REQUEST == _IOW('f', 217, struct hsm_user_request));
CHECK(LL_SUPER_MAGIC == 0x0BD00BD0);
CHECK(LOV_USER_MAGIC_V1 == 0x0BD10BD0);
CHECK(LOV_USER_MAGIC_V3 == 0x0BD30BD0);
CHECK(LOV_USER_MAGIC_COMP_V1 == 0x0BD60BD0);
CHECK(HS_EXISTS == 0x01 && HS_DIRTY == 0x02 && HS_RELEASED == 0x04 && HS_ARCHIVED == 0x08);
CHECK(HS_NORELEASE == 0x10 && HS_NOARCHIVE == 0x20 && HS_LOST == 0x40);
CHECK(HUA_RESTORE == 11);
CHECK(HPS_NONE == 0 && HPS_WAITING == 1 && HPS_RUNNING == 2 && HPS_DONE == 3);
CHECK(FILEID_LUSTRE == 0x97);
int main(void) { return 0; }
CHK
CC="${CC:-cc}"
if "$CC" -std=gnu11 -D_GNU_SOURCE -I "$INC" -fsyntax-only "$TMP/check.c"; then
  echo "check_llapi_abi: Lustre uapi constants match ($HDR)"
else
  echo "check_llapi_abi: uapi drift against $HDR" >&2
  exit 1
fi
