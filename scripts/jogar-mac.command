#!/bin/bash
# Abre o cliente macOS (bin/Mac/World of Warcraft.app). Dá para abrir com dois cliques no Finder.
#
# - Se o .app ainda não existe, configura e compila antes (CMake + Ninja em build-mac/).
# - Com --build, recompila mesmo que o .app já exista.
# - O jogo procura Data/, WTF/ e Interface/ ao lado do .app, em bin/Mac/.
# - Compila com WOWDECOMP_DISABLE_SIGNATURE_CHECKS=ON, como o cliente Windows usado com os patches de tradução
#   (patch-6/patch-7): sem isso o cliente recusa a interface de login modificada.

set -euo pipefail

ROOT="$(cd "$(dirname "$0")/.." && pwd)"
APP="$ROOT/bin/Mac/World of Warcraft.app"
BUILD="$ROOT/build-mac"

cd "$ROOT"

if [ ! -d "$APP" ] || [ "${1:-}" = "--build" ]; then
    echo "Compilando o cliente (pode levar alguns minutos na primeira vez)..."
    if [ ! -f "$BUILD/build.ninja" ]; then
        cmake -S "$ROOT" -B "$BUILD" -G Ninja -DCMAKE_BUILD_TYPE=Debug -DWOWDECOMP_DISABLE_SIGNATURE_CHECKS=ON
    fi
    cmake --build "$BUILD" --target Wow
fi

if [ ! -e "$ROOT/bin/Mac/Data" ]; then
    echo "Não achei bin/Mac/Data."
    echo "Crie um link para a pasta Data do cliente 3.3.5a, por exemplo:"
    echo "  ln -s \"/caminho/do/World of Warcraft 3.3.5a/Data\" \"$ROOT/bin/Mac/Data\""
    exit 1
fi

echo "Abrindo o jogo. Logs: bin/Mac/Logs e bin/Mac/Errors."
open "$APP"
