#!/usr/bin/env bash
# Download the first N MB of a public NASDAQ TotalView-ITCH 5.0 sample file
# and decompress that prefix to data/itch/sample.itch.
#
# The full files are several GB, so we fetch a byte range of the gzip and
# decompress what is there. gunzip stops at the cut with "unexpected end of
# file", which is expected; the parser ignores the final partial message.
# The data is NASDAQ's and is not committed (size and license); RESULTS.md
# records its source, byte count and sha256 so the run can be reproduced.
#
# Usage: tools/fetch_itch_sample.sh [MB=64] [FILE=01302019.NASDAQ_ITCH50.gz]
set -euo pipefail

MB="${1:-64}"
FILE="${2:-01302019.NASDAQ_ITCH50.gz}"
URL="https://emi.nasdaq.com/ITCH/Nasdaq%20ITCH/${FILE}"
ROOT="$(cd "$(dirname "$0")/.." && pwd)"
OUT="${ROOT}/data/itch"
mkdir -p "${OUT}"

echo "fetching first ${MB} MB of ${URL}"
curl -sS --fail -r "0-$((MB * 1024 * 1024 - 1))" -o "${OUT}/sample.itch.gz" "${URL}"
# gunzip exits non-zero at the truncated end; keep whatever it produced.
gunzip -c "${OUT}/sample.itch.gz" > "${OUT}/sample.itch" 2>/dev/null || true
rm -f "${OUT}/sample.itch.gz"

{
  echo "source=${URL}"
  echo "gzip_prefix_mb=${MB}"
  echo "bytes=$(wc -c < "${OUT}/sample.itch" | tr -d ' ')"
  echo "sha256=$(shasum -a 256 "${OUT}/sample.itch" | cut -d' ' -f1)"
} > "${OUT}/sample.meta"
cat "${OUT}/sample.meta"
