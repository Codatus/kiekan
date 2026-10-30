#!/bin/bash
# KiekAn! – portables Windows-Paket aus einem MSYS2-UCRT64-Build erstellen.
# Aufruf (in der MSYS2-Shell): packaging/windows/deploy.sh <build-ordner> <ziel-ordner>
#
# Ergebnis: ein Ordner mit kiekan.exe, allen benötigten DLLs, Qt-Plugins,
# den KiekAn!-Bildformat- und Filter-Plugins und Tesseract-Sprachdaten.
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

# Tesseract-Sprachdaten (KiekAn! findet sie in <Programmordner>/tessdata)
for lang in osd eng deu frk; do
    cp "$PREFIX/share/tessdata/$lang.traineddata" "$OUT/tessdata/"
done

# Lizenztexte aller mitgelieferten Bibliotheken (Pflicht bei Weitergabe):
# zu jeder DLL das MSYS2-Paket ermitteln und dessen Lizenzordner kopieren.
mkdir -p "$OUT/licenses"
# (Die Kopien gehören keinem Paket – daher das Original in MSYS2 suchen.)
pkgs=$( { find "$OUT" -iname '*.dll' -printf '%f\n' | sort -u | while read -r name; do
              orig=$(find "$PREFIX/bin" "$PREFIX/share/qt6/plugins" -iname "$name" 2>/dev/null | head -n 1)
              [ -n "$orig" ] && pacman -Qqo "$orig" 2>/dev/null || true
          done
          for p in tesseract-data-eng tesseract-data-deu tesseract-data-frk; do echo "${MINGW_PACKAGE_PREFIX}-$p"; done; } | sort -u)
: > "$OUT/licenses/VERSIONEN.txt"
for pkg in $pkgs; do
    pacman -Q "$pkg" >> "$OUT/licenses/VERSIONEN.txt" 2>/dev/null || continue
    short=${pkg#"${MINGW_PACKAGE_PREFIX}-"}
    if [ -d "$PREFIX/share/licenses/$short" ]; then
        cp -r "$PREFIX/share/licenses/$short" "$OUT/licenses/"
    else
        # Ohne eigenen Lizenzordner: Lizenzangabe aus der Paketbeschreibung
        pacman -Qi "$pkg" | grep -E '^(Name|Version|Licenses|URL)' > "$OUT/licenses/$short.txt"
    fi
done
cp packaging/DRITTANBIETER.md "$OUT/licenses/"

echo "Paket erstellt in $OUT:"
du -sh "$OUT"
ls "$OUT" | head -50
