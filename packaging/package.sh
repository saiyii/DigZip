#!/usr/bin/env bash
# Builds the release archives from a finished build, into dist/.
#
#   packaging/package.sh <version> <target> <build-dir> <vcpkg-triplet> [cli] [gui]
#
# cli: the standalone command-line tool. gui: the graphical interface with the Qt libraries
# (windeployqt or macdeployqt must be on PATH). Needs 7z on Windows.
set -euo pipefail

version=$1 target=$2 build=$3 triplet=$4
shift 4
root=$(cd "$(dirname "$0")/.." && pwd)
build=$(cd "$build" && pwd)
mkdir -p "$root/dist"
dist=$(cd "$root/dist" && pwd)

case "$(uname -s)" in
    MINGW* | MSYS* | CYGWIN*) system=windows ;;
    Darwin) system=macos ;;
    *) system=linux ;;
esac

find_binary() {
    find "$build/bin" -name "$1" -type f -not -path "*.app/*" | head -n 1
}

add_documents() {
    mkdir -p "$1/licenses"
    cp "$root"/{README.md,CHANGELOG.md,THIRD_PARTY.md,LICENSE-MIT,LICENSE-APACHE} "$1/"
    for copyright in "$build/vcpkg_installed/$triplet"/share/*/copyright; do
        port=$(basename "$(dirname "$copyright")")
        [ "$port" = catch2 ] && continue
        cp "$copyright" "$1/licenses/$port.txt"
    done
}

archive() {
    (
        cd "$dist"
        case "$system" in
            windows) 7z a -tzip "$1.zip" "$1" > /dev/null ;;
            macos) ditto -c -k --keepParent "$1" "$1.zip" ;; # keeps the symbolic links of the frameworks
            linux) tar -czf "$1.tar.gz" "$1" ;;
        esac
        rm -rf "$1"
    )
}

# The Visual C++ runtime DLLs, deployed next to the program as Microsoft allows.
copy_msvc_runtime() {
    local vswhere vs crt
    vswhere="$(printenv 'ProgramFiles(x86)')/Microsoft Visual Studio/Installer/vswhere.exe"
    vs=$("$vswhere" -latest -all -products '*' -property installationPath | tr -d '\r')
    crt=$(ls -d "$vs"/VC/Redist/MSVC/*/x64/Microsoft.VC14*.CRT | sort -V | tail -n 1)
    cp "$crt"/{msvcp140.dll,msvcp140_1.dll,vcruntime140.dll,vcruntime140_1.dll} "$1/"
}

package_cli() {
    local name="digzip-$version-$target" binary
    binary=$(find_binary "digzip$([ $system = windows ] && echo .exe)")
    mkdir -p "$dist/$name"
    cp "$binary" "$dist/$name/"
    add_documents "$dist/$name"
    archive "$name"
}

package_gui() {
    local name="digzip-gui-$version-$target"
    local dir="$dist/$name"
    mkdir -p "$dir"
    case "$system" in
        windows)
            cp "$(find_binary digzip-gui.exe)" "$(find_binary digzip.exe)" "$dir/"
            windeployqt --release --translations en,fr --no-system-d3d-compiler --no-system-dxc-compiler \
                --no-opengl-sw --no-compiler-runtime --no-ffmpeg --no-network \
                --skip-plugin-types tls,networkinformation,generic "$dir/digzip-gui.exe" > /dev/null
            # Qt uses the ICU of Windows (10 and later), which windeployqt copies from the system.
            rm -f "$dir/icuuc.dll" "$dir/icuin.dll"
            copy_msvc_runtime "$dir"
            ;;
        macos)
            cp -R "$(find "$build/bin" -maxdepth 2 -name DigZip.app -type d | head -n 1)" "$dir/"
            macdeployqt "$dir/DigZip.app" -always-overwrite
            # Qt's own texts (standard buttons and dialogs) in the languages of the interface.
            mkdir -p "$dir/DigZip.app/Contents/Resources/translations"
            cp "$(qtpaths --query QT_INSTALL_TRANSLATIONS)"/qtbase_{en,fr}.qm "$dir/DigZip.app/Contents/Resources/translations/"
            codesign --force --deep --sign - "$dir/DigZip.app" # ad-hoc signature, required on Apple silicon
            ;;
        linux)
            echo "No graphical package for Linux yet" >&2
            exit 1
            ;;
    esac
    add_documents "$dir"
    cp "$root"/packaging/licenses/*.txt "$dir/licenses/"
    archive "$name"
}

for part in "$@"; do
    case "$part" in
        cli) package_cli ;;
        gui) package_gui ;;
        *) echo "unknown part: $part" >&2; exit 2 ;;
    esac
done
ls -l "$dist"
