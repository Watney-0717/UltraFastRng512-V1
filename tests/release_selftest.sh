#!/usr/bin/env bash
set -euo pipefail
ROOT=$(cd "$(dirname "$0")/.." && pwd)
"$ROOT/tools/build_and_verify.sh"
[[ -f "$ROOT/docs/UltraFastRng512_V1_DECONSTRUCTION_BOOK_v3.3_EN_COMPLETE_CC_BY_NC_ND_20260930.pdf" ]]
[[ -f "$ROOT/LICENSE-AGPL-3.0-only.txt" ]]
[[ -f "$ROOT/COMMERCIAL_LICENSE.md" ]]
[[ -f "$ROOT/COMMERCIAL-LICENSE-NOTICE.md" ]]
[[ -f "$ROOT/THIRD_PARTY_LICENSES.md" ]]
[[ -f "$ROOT/license/BENCHMARK-SOURCE-0BSD.txt" ]]
[[ -f "$ROOT/license/DOCUMENTATION-CC-BY-NC-ND-4.0.md" ]]
[[ -f "$ROOT/license/MATHEMATICAL-SPEC-CC-BY-4.0.md" ]]
[[ -f "$ROOT/license/RESULTS-CC0-1.0.md" ]]
grep -q 'Watney-0717' "$ROOT/SOURCE_LICENSE_MAP.md"
! find "$ROOT" -type f \( -name '*V2*' -o -name '*LEGACY*' -o -name '*staging*' -o -name '*POC*' -o -name '*RECOVERY*' \) | grep -v '/tests/'
printf 'RELEASE_SELFTEST: PASS\n'
