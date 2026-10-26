#!/bin/bash
# Kiek an – portables Windows-Paket aus einem MSYS2-UCRT64-Build erstellen.
# Aufruf (in der MSYS2-Shell): packaging/windows/deploy.sh <build-ordner> <ziel-ordner>
#
# Ergebnis: ein Ordner mit kiekan.exe, allen benötigten DLLs, Qt-Plugins,
# den Kiek-an-Bildformat- und Filter-Plugins und Tesseract-Sprachdaten.
set -euo pipefail

BUILD="$1"
OUT="$2"
PREFIX="${MINGW_PREFIX:-/ucrt64}"

rm -rf "$OUT"
mkdir -p "$OUT/plugins" "$OUT/tessdata"

cp "$BUILD/kiekan.exe" "$OUT/"
cp -r "$BUILD/qtplugins" "$OUT/"                       # HEIC, RAW, JPEG 2000, DjVu, PSD
cp "$BUILD"/plugins/*.dll "$OUT/plugins/" 2>/dev/null || true   # Filter-Plugins (Beispiel „Rahmen“)
cp README.md LICENSE "$OUT/"

# Qt-Bibliotheken, Plattform-, Stil-, Bildformat- und Übersetzungsdateien
WINDEPLOYQT=$(command -v windeployqt6 || command -v windeployqt-qt6 || command -v windeployqt)
"$WINDEPLOYQT" --release --no-compiler-runtime --dir "$OUT" "$OUT/kiekan.exe"
# MCP-Server und OCR-Hilfsprozesse laufen ohne Fenster (QT_QPA_PLATFORM=offscreen);
# windeployqt kopiert nur das Plattform-Plugin „windows“.
QTPLUGINS=$(qtpaths-qt6 --query QT_INSTALL_PLUGINS 2>/dev/null || qtpaths6 --query QT_INSTALL_PLUGINS 2>/dev/null || echo "$PREFIX/share/qt6/plugins")
cp "$(cygpath -u "$QTPLUGINS")/platforms/qoffscreen.dll" "$OUT/platforms/"

# Alle übrigen DLLs aus MSYS2 einsammeln (mpv, exiv2, Tesseract, qpdf, libheif …),
# auch die, die erst die Plugins brauchen. Wiederholen, bis nichts Neues dazukommt.
while true; do
    added=0
    while IFS= read -r -d '' f; do
        while read -r dll; do
            [ -z "$dll" ] && continue
            name=$(basename "$dll")
            if [ ! -e "$OUT/$name" ]; then
                cp "$dll" "$OUT/"
                added=1
            fi
        done < <(ntldd -R "$f" 2>/dev/null | awk '{print $3}' | tr '\\' '/' | grep -i "/ucrt64/bin/" | while read -r p; do cygpath -u "$p"; done | sort -u)
    done < <(find "$OUT" \( -name '*.exe' -o -name '*.dll' \) -print0)
    [ "$added" = 0 ] && break
done

# Tesseract-Sprachdaten (Kiek an findet sie in <Programmordner>/tessdata)
for lang in osd eng deu frk; do
    cp "$PREFIX/share/tessdata/$lang.traineddata" "$OUT/tessdata/"
done

echo "Paket erstellt in $OUT:"
du -sh "$OUT"
ls "$OUT" | head -50
