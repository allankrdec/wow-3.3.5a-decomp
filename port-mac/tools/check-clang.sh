#!/usr/bin/env bash
# Syntax-checks the platform-independent .cpp files with the host clang (macOS arm64).
# Stage E1 is done when every file passes. See port-mac/docs/01-metricas-e-experimento-clang.md.
#
# usage: port-mac/tools/check-clang.sh [-j jobs] [-o outdir] [file.cpp ...]
#   no files: checks every .cpp of Engine/Source and WoW outside the Windows-only folders and mac-excluded.txt.
#   outdir (default: build-check-clang) gets ok.txt, fail.txt and one .log per failing file under logs/.

set -euo pipefail

root="$(cd "$(dirname "$0")/../.." && pwd)"
cd "$root"

jobs="$(sysctl -n hw.ncpu 2>/dev/null || nproc)"
out="build-check-clang"
while getopts "j:o:" opt; do
    case "$opt" in
        j) jobs="$OPTARG" ;;
        o) out="$OPTARG" ;;
        *) sed -n '2,8p' "$0"; exit 2 ;;
    esac
done
shift $((OPTIND - 1))

mkdir -p "$out"
out="$(cd "$out" && pwd)"
rm -rf "$out/logs" "$out/results"
mkdir -p "$out/logs" "$out/results"

if [ $# -gt 0 ]; then
    printf '%s\n' "$@" > "$out/files.txt"
else
    find Engine/Source WoW -name '*.cpp' \
        | grep -vE '/(Win32|Win64|win32|Windows)/|/fmodex/|/CGxDevice(D3d|D3d9Ex|OpenGL)/' \
        | grep -vEf <(grep -vE '^(#|$)' port-mac/tools/mac-excluded.txt) \
        | sort > "$out/files.txt"
fi

compat="Engine/Source/Base/Platform/Compat.h"
flags=(
    -std=c++17 -fsyntax-only -w
    # MSVC accepts narrowing conversions in braced initializers (sizeof sums into uint32_t arrays).
    -Wno-c++11-narrowing
    -DWOWDECOMP_ARCH_64=1 -DNOMINMAX
    -I. -IEngine/Source/Base/Platform/Mac/Include -IThirdParty/zlib -IThirdParty/expat/lib -IThirdParty/freetype/include -IThirdParty/pcre
    -IEngine/Source/SoundEngine/fmodex/lib/ogg_vorbis/ogg/include
    -IEngine/Source/SoundEngine/fmodex/lib/ogg_vorbis/vorbis/include
)
if [ -f "$compat" ]; then
    flags+=(-include "$compat")
fi

export out
check_one() {
    local f="$1"; shift
    local log="$out/logs/$(echo "$f" | tr '/' '_').log"
    local res="$out/results/$(echo "$f" | tr '/' '_')"
    if clang++ "$@" "$f" > "$log" 2>&1; then
        rm -f "$log"
        echo "OK $f" > "$res"
    else
        echo "FAIL $f" > "$res"
    fi
}
export -f check_one

tr '\n' '\0' < "$out/files.txt" \
    | xargs -0 -P "$jobs" -I{} bash -c 'check_one "$@"' _ {} "${flags[@]}"

cat "$out/results/"* | sort -k2 > "$out/all.txt"
grep '^OK ' "$out/all.txt" | cut -d' ' -f2- > "$out/ok.txt" || true
grep '^FAIL ' "$out/all.txt" | cut -d' ' -f2- > "$out/fail.txt" || true

total=$(wc -l < "$out/files.txt" | tr -d ' ')
ok=$(wc -l < "$out/ok.txt" | tr -d ' ')
fail=$(wc -l < "$out/fail.txt" | tr -d ' ')
echo "check-clang: $ok/$total OK, $fail FAIL (results in $out)"
[ "$fail" -eq 0 ]
