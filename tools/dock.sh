#!/usr/bin/env bash
# usage: tools/dock.sh <image> '<bash command>'   (optional env DOCK_ARGS = extra docker run args)
set -u
[ $# -ge 2 ] || { echo "usage: $0 <image> '<bash command>'" >&2; exit 2; }
img=$1; shift
here=$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && { pwd -W 2>/dev/null || pwd; })
# shellcheck disable=SC2086
MSYS_NO_PATHCONV=1 docker run --rm ${DOCK_ARGS:-} \
  -v "$here:/src" -v gktwo-work:/work -w /src "$img" bash -c "$*"
