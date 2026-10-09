#!/bin/sh
# Prépare la compilation croisée Linux -> Windows x64 (cmake/clang-cl-xwin.cmake) :
# télécharge xwin, puis le SDK Windows et le CRT MSVC (x64, Release) dans $XWIN_DIR.
# Prérequis : clang (clang-cl), lld (lld-link), cmake, ninja, git, curl.
# xwin télécharge les paquets de Microsoft : leur licence (celle de Visual Studio Build Tools)
# est acceptée par --accept-license.
set -eu

XWIN_VERSION=0.10.0
XWIN_DIR=${XWIN_DIR:-$HOME/.local/opt/winsdk}
TOOLS_DIR=${TOOLS_DIR:-$HOME/.local/opt}

for tool in clang-cl lld-link cmake ninja git curl; do
    command -v "$tool" >/dev/null || { echo "manquant : $tool" >&2; exit 1; }
done

case $(uname -m) in
    aarch64|arm64) arch=aarch64 ;;
    x86_64) arch=x86_64 ;;
    *) echo "architecture non gérée : $(uname -m)" >&2; exit 1 ;;
esac

name=xwin-$XWIN_VERSION-$arch-unknown-linux-musl
xwin=$TOOLS_DIR/$name/xwin
if [ ! -x "$xwin" ]; then
    mkdir -p "$TOOLS_DIR"
    url=https://github.com/Jake-Shadle/xwin/releases/download/$XWIN_VERSION/$name.tar.gz
    curl -fsSL "$url" -o "$TOOLS_DIR/$name.tar.gz"
    expected=$(curl -fsSL "$url.sha256" | cut -d' ' -f1)
    actual=$(sha256sum "$TOOLS_DIR/$name.tar.gz" | cut -d' ' -f1)
    [ "$expected" = "$actual" ] || { echo "somme de contrôle xwin incorrecte" >&2; exit 1; }
    tar xzf "$TOOLS_DIR/$name.tar.gz" -C "$TOOLS_DIR"
    rm "$TOOLS_DIR/$name.tar.gz"
fi

"$xwin" --accept-license --arch x86_64 --cache-dir "${XDG_CACHE_HOME:-$HOME/.cache}/xwin" \
    splat --copy --use-winsysroot-style --output "$XWIN_DIR"

echo "SDK Windows prêt dans $XWIN_DIR"
echo "Compiler :"
echo "  cmake -S . -B build-linux -G Ninja -DCMAKE_BUILD_TYPE=Release -DCMAKE_TOOLCHAIN_FILE=cmake/clang-cl-xwin.cmake"
echo "  cmake --build build-linux"
