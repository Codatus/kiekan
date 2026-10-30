#!/bin/bash
# KiekAn! – macOS-Programmpaket (KiekAn.app und .dmg) aus einem Homebrew-Build.
# Aufruf: packaging/macos/deploy.sh <build-ordner> <ziel-ordner> <version>
#
# Ergebnis: KiekAn.app mit allen Bibliotheken, Qt-Plugins, den eigenen
# Bildformat- und Filter-Plugins und Tesseract-Sprachdaten, ad hoc signiert.
set -euo pipefail

BUILD="$1"
OUT="$2"
VERSION="$3"
APP="$OUT/KiekAn.app"
BREW=$(brew --prefix)

rm -rf "$APP"
mkdir -p "$APP/Contents/MacOS/qtplugins" "$APP/Contents/MacOS/tessdata" \
         "$APP/Contents/lib/kiekan/plugins" "$APP/Contents/Resources"

cp "$BUILD/kiekan" "$APP/Contents/MacOS/"
cp -R "$BUILD/qtplugins/." "$APP/Contents/MacOS/qtplugins/"          # HEIC, RAW, JPEG 2000, DjVu, PSD
cp "$BUILD"/plugins/*.dylib "$APP/Contents/lib/kiekan/plugins/" 2>/dev/null || true
# Homebrew bringt nur Englisch und osd mit; Deutsch fest nach Prüfsumme
for lang in osd eng; do
    cp "$BREW/share/tessdata/$lang.traineddata" "$APP/Contents/MacOS/tessdata/"
done
curl -sSfL -o "$APP/Contents/MacOS/tessdata/deu.traineddata" \
    https://raw.githubusercontent.com/tesseract-ocr/tessdata/ced78752cc61322fb554c280d13360b35b8684e4/deu.traineddata
echo "896b3b4956503ab9daa10285db330881b2d74b70d889b79262cc534b9ec699a4  $APP/Contents/MacOS/tessdata/deu.traineddata" | shasum -a 256 -c

# Symbol: SVG → icns
ICONSET=$(mktemp -d)/kiekan.iconset
mkdir -p "$ICONSET"
for s in 16 32 128 256 512; do
    rsvg-convert -w $s -h $s data/kiekan.svg -o "$ICONSET/icon_${s}x${s}.png"
    rsvg-convert -w $((s * 2)) -h $((s * 2)) data/kiekan.svg -o "$ICONSET/icon_${s}x${s}@2x.png"
done
iconutil -c icns "$ICONSET" -o "$APP/Contents/Resources/kiekan.icns"

cat > "$APP/Contents/Info.plist" <<PLIST
<?xml version="1.0" encoding="UTF-8"?>
<!DOCTYPE plist PUBLIC "-//Apple//DTD PLIST 1.0//EN" "http://www.apple.com/DTDs/PropertyList-1.0.dtd">
<plist version="1.0">
<dict>
    <key>CFBundleExecutable</key><string>kiekan</string>
    <key>CFBundleIdentifier</key><string>io.github.codatus.kiekan</string>
    <key>CFBundleName</key><string>KiekAn!</string>
    <key>CFBundleDisplayName</key><string>KiekAn!</string>
    <key>CFBundleIconFile</key><string>kiekan</string>
    <key>CFBundlePackageType</key><string>APPL</string>
    <key>CFBundleShortVersionString</key><string>$VERSION</string>
    <key>CFBundleVersion</key><string>$VERSION</string>
    <key>LSMinimumSystemVersion</key><string>13.0</string>
    <key>NSHighResolutionCapable</key><true/>
    <key>CFBundleDocumentTypes</key>
    <array>
        <dict>
            <key>CFBundleTypeName</key><string>Image</string>
            <key>CFBundleTypeRole</key><string>Viewer</string>
            <key>LSHandlerRank</key><string>Alternate</string>
            <key>LSItemContentTypes</key>
            <array>
                <string>public.image</string>
                <string>public.camera-raw-image</string>
                <string>com.adobe.pdf</string>
                <string>public.movie</string>
            </array>
        </dict>
    </array>
</dict>
</plist>
PLIST

# Qt und alle Homebrew-Bibliotheken einsammeln, auch die der Plugins
args=()
for f in "$APP"/Contents/MacOS/qtplugins/*/*.dylib "$APP"/Contents/lib/kiekan/plugins/*.dylib; do
    [ -e "$f" ] && args+=("-executable=$f")
done
# -libpath: Homebrew verlinkt alle Bibliotheken und Qt-Frameworks nach $BREW/lib;
# ohne das bleiben @rpath-Verweise (z. B. QtSvg, brotli) ungelöst.
macdeployqt "$APP" "${args[@]}" -libpath="$BREW/lib" -verbose=1
# MCP-Server und Texterkennung laufen ohne Fenster
cp "$(qtpaths6 --query QT_INSTALL_PLUGINS)/platforms/libqoffscreen.dylib" "$APP/Contents/PlugIns/platforms/" 2>/dev/null || true
macdeployqt "$APP" -executable="$APP/Contents/PlugIns/platforms/libqoffscreen.dylib" -libpath="$BREW/lib" -verbose=0 || true

# macdeployqt löst nicht alle @rpath-Verweise auf (Homebrew verteilt Qt auf
# viele Formeln). Fehlende Bibliotheken selbst ins Paket holen und alle
# Verweise auf @rpath umstellen, bis nichts mehr nach außen zeigt.
FW="$APP/Contents/Frameworks"
macho() { find "$APP/Contents" -type f -print0 | while IFS= read -r -d '' f; do
              file -b "$f" | grep -q 'Mach-O' && printf '%s\0' "$f"; done; }
relname() {   # Pfad im Frameworks-Ordner für einen Verweis
    case "$1" in
        *.framework/*) echo "$1" | sed -E 's|.*/([^/]+\.framework/.*)|\1|' ;;
        *) basename "$1" ;;
    esac
}
fetch() {     # Bibliothek aus Homebrew ins Paket kopieren
    local rel="$1" top src
    top=${rel%%/*}
    [ -e "$FW/$top" ] && return 0
    src=$(ls -d "$BREW/lib/$top" "$BREW"/opt/*/lib/"$top" 2>/dev/null | head -n 1)
    [ -n "$src" ] || { echo "Nicht gefunden: $rel"; return 1; }
    cp -RH "$src" "$FW/"
    chmod -R u+w "$FW/$top"
    install_name_tool -id "@rpath/$rel" "$FW/$rel" 2>/dev/null
    echo "Ergänzt: $rel"
}
ensure_rpath() {   # @rpath relativ zur Datei auf den Frameworks-Ordner zeigen lassen
    local rp="@loader_path/$(python3 -c 'import os,sys;print(os.path.relpath(sys.argv[1],os.path.dirname(sys.argv[2])))' "$FW" "$1")"
    otool -l "$1" | grep -q "path $rp " || install_name_tool -add_rpath "$rp" "$1" 2>/dev/null || true
}
for pass in 1 2 3 4 5 6 7 8; do
    changed=0
    while IFS= read -r -d '' f; do
        for ref in $(otool -L "$f" | tail -n +2 | awk '{print $1}'); do
            case "$ref" in
                "$BREW"/*|/usr/local/*|@rpath/*)
                    rel=$(relname "${ref#@rpath/}")
                    [ -e "$FW/$rel" ] || { fetch "$rel" && changed=1; }
                    if [ "$ref" != "@rpath/$rel" ]; then
                        install_name_tool -change "$ref" "@rpath/$rel" "$f" 2>/dev/null && changed=1
                    fi
                    ensure_rpath "$f" ;;
            esac
        done
    done < <(macho)
    [ "$changed" = 0 ] && break
done

# Prüfen: jeder Verweis zeigt ins Paket oder auf das System
bad=0
while IFS= read -r -d '' f; do
    for ref in $(otool -L "$f" | tail -n +2 | awk '{print $1}'); do
        case "$ref" in
            @rpath/*) [ -e "$FW/${ref#@rpath/}" ] || { echo "Fehlt im Paket: $ref ($f)"; bad=1; } ;;
            /usr/lib/*|/System/*|@executable_path/*|@loader_path/*) ;;
            *) echo "Verweist nach außen: $ref ($f)"; bad=1 ;;
        esac
    done
done < <(macho)
[ "$bad" = 0 ] || exit 1
echo "Alle Verweise im Paket aufgelöst ($(ls "$FW" | wc -l | tr -d ' ') Bibliotheken)"

# Lizenztexte aller mitgelieferten Bibliotheken (Pflicht bei Weitergabe):
# zu jeder Bibliothek im Paket die Homebrew-Formel suchen.
LIC="$APP/Contents/Resources/licenses"
mkdir -p "$LIC"
formulas=$(for f in "$FW"/*; do
               name=$(basename "$f")
               ls -d "$BREW"/Cellar/*/*/lib/"$name" 2>/dev/null | head -n 1 \
                   | sed -E "s|^$BREW/Cellar/([^/]+)/.*|\1|" || true
           done | sort -u)
for formula in $formulas tesseract; do
    dir=$(brew --prefix "$formula" 2>/dev/null) || continue
    mkdir -p "$LIC/$formula"
    find -L "$dir" -maxdepth 1 -type f \( -iname 'LICEN[CS]E*' -o -iname 'COPYING*' -o -iname 'COPYRIGHT*' -o -iname 'NOTICE*' \) \
        -exec cp {} "$LIC/$formula/" \;
    # Ohne Lizenzdatei im Paketordner: Angabe aus Homebrew
    [ -n "$(ls "$LIC/$formula")" ] || brew info --json=v2 "$formula" \
        | python3 -c "import json,sys;f=json.load(sys.stdin)['formulae'][0];print(f['name'],f['versions']['stable'],'License:',f.get('license'),f.get('homepage'))" \
        > "$LIC/$formula/LIZENZ.txt"
    brew list --versions "$formula" >> "$LIC/VERSIONEN.txt"
done
cp packaging/DRITTANBIETER.md "$LIC/"
echo "Lizenzen von $(ls "$LIC" | wc -l) Formeln: $(ls "$LIC" | tr '\n' ' ')"

# Apple Silicon verlangt Signaturen; ohne Entwicklerkonto ad hoc
codesign --force --deep --sign - "$APP"

hdiutil create -volname "KiekAn!" -srcfolder "$APP" -ov -format UDZO "$OUT/KiekAn-$VERSION-macos-arm64.dmg"
du -sh "$APP"
