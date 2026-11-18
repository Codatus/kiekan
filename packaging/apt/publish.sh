#!/bin/bash
# KiekAn! – .deb-Pakete in die apt-Paketquelle (GitHub Pages, Zweig gh-pages) einpflegen.
# Aufruf: packaging/apt/publish.sh <ordner-mit-debs> <gh-pages-checkout>
#
# Braucht apt-ftparchive (apt-utils), dpkg-deb und gpg mit dem Signierschlüssel
# im Schlüsselbund (im Workflow aus dem Secret APT_SIGNING_KEY). Die Quelle
# liegt danach unter <checkout>/apt: pool/<suite>/*.deb und dists/<suite>/…
# Je Distribution und Architektur bleiben die neuesten KEEP Versionen.
set -euo pipefail

DEBS=$(realpath "$1")
PAGES=$(realpath "$2")
HERE=$(dirname "$(realpath "$0")")
URL=https://codatus.github.io/kiekan/apt
KEEP=3
SUITES="jammy noble bookworm trixie"
ARCHS="amd64 arm64"

# Distribution im Dateinamen (KIEKAN_PKG_SUFFIX) → Suite
suite_for() {
    case "$1" in
        *_ubuntu22.04_*) echo jammy ;;
        *_ubuntu24.04_*) echo noble ;;
        *_debian12_*) echo bookworm ;;
        *_debian13_*) echo trixie ;;
        *) return 1 ;;
    esac
}
describe() {
    case "$1" in
        jammy) echo "Ubuntu 22.04" ;; noble) echo "Ubuntu 24.04" ;;
        bookworm) echo "Debian 12" ;; trixie) echo "Debian 13" ;;
    esac
}

REPO="$PAGES/apt"
mkdir -p "$REPO"

# 1. Neue Pakete in pool/<suite>/ übernehmen
found=0
for deb in "$DEBS"/*.deb; do
    [ -e "$deb" ] || continue
    name=$(basename "$deb")
    if ! suite=$(suite_for "$name"); then
        echo "Übersprungen (keine bekannte Distribution): $name"
        continue
    fi
    mkdir -p "$REPO/pool/$suite"
    cp "$deb" "$REPO/pool/$suite/"
    echo "Übernommen: $suite/$name"
    found=$((found + 1))
done
[ "$found" -gt 0 ] || { echo "Keine passenden .deb-Pakete in $DEBS" >&2; exit 1; }

# 2. Je Suite und Architektur nur die neuesten KEEP Versionen behalten
for suite in $SUITES; do
    for arch in $ARCHS; do
        [ -d "$REPO/pool/$suite" ] || continue
        files=()
        for f in "$REPO/pool/$suite"/*_"$arch".deb; do
            [ -e "$f" ] && files+=("$f")
        done
        # nach Debian-Versionsvergleich sortieren, neueste zuerst
        sorted=()
        for f in "${files[@]}"; do
            v=$(dpkg-deb -f "$f" Version)
            i=0
            while [ "$i" -lt "${#sorted[@]}" ] \
                  && dpkg --compare-versions "$(dpkg-deb -f "${sorted[$i]}" Version)" gt "$v"; do
                i=$((i + 1))
            done
            sorted=("${sorted[@]:0:$i}" "$f" "${sorted[@]:$i}")
        done
        for f in "${sorted[@]:$KEEP}"; do
            echo "Entfernt (älter als die neuesten $KEEP): $suite/$(basename "$f")"
            rm -f -- "${f:?}"
        done
    done
done

# 3. Indizes und signierte Release-Dateien je Suite
cd "$REPO"
rm -rf -- "${REPO:?}/dists"
for suite in $SUITES; do
    [ -d "pool/$suite" ] || continue
    for arch in $ARCHS; do
        out="dists/$suite/main/binary-$arch"
        mkdir -p "$out"
        apt-ftparchive --arch "$arch" packages "pool/$suite" > "$out/Packages"
        gzip -9kn "$out/Packages"
    done
    apt-ftparchive \
        -o APT::FTPArchive::Release::Origin="KiekAn!" \
        -o APT::FTPArchive::Release::Label="KiekAn!" \
        -o APT::FTPArchive::Release::Suite="$suite" \
        -o APT::FTPArchive::Release::Codename="$suite" \
        -o APT::FTPArchive::Release::Architectures="$ARCHS" \
        -o APT::FTPArchive::Release::Components=main \
        -o APT::FTPArchive::Release::Description="KiekAn! Bildbetrachter für $(describe "$suite")" \
        release "dists/$suite" > "dists/$suite/Release.tmp"
    mv "dists/$suite/Release.tmp" "dists/$suite/Release"
    gpg --batch --yes --clearsign -o "dists/$suite/InRelease" "dists/$suite/Release"
    gpg --batch --yes --armor --detach-sign -o "dists/$suite/Release.gpg" "dists/$suite/Release"
done

# 4. Schlüssel, fertige Quellen-Dateien und Startseite
cp "$HERE/kiekan-archive-keyring.gpg" "$HERE/kiekan-archive-keyring.asc" "$REPO/"
for suite in $SUITES; do
    sed -e "s|@KIEKAN_APT_URL@|$URL|" -e "s|@KIEKAN_APT_SUITE@|$suite|" \
        "$HERE/kiekan.sources.in" > "$REPO/kiekan-$suite.sources"
done
cp "$HERE/index.html" "$PAGES/index.html"
touch "$PAGES/.nojekyll"

echo "Stand der Paketquelle:"
find "$REPO/pool" -name '*.deb' -printf '  %P\n' | sort
