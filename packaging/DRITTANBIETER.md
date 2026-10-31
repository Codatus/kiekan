# KiekAn! – Bestandteile von Drittanbietern

KiekAn! selbst steht unter der GNU General Public License, Version 3 oder
später (Datei `LICENSE`). Der vollständige Quelltext liegt unter
https://github.com/Codatus/kiekan – für jede veröffentlichte Version unter dem
gleichnamigen Tag (z. B. `v0.13.0`).

Das Windows-ZIP und das macOS-Programmpaket enthalten zusätzlich fertig
übersetzte Bibliotheken anderer Projekte. Ihre Lizenztexte liegen im Ordner
`licenses` (Windows: neben `kiekan.exe`; macOS: `KiekAn.app/Contents/Resources/licenses`),
die genauen Paketversionen in `licenses/VERSIONEN.txt`. Die wichtigsten:

| Bibliothek | Zweck | Lizenz |
|---|---|---|
| Qt 6 | Oberfläche, Bildformate, PDF-Anzeige (QtPdf/PDFium) | LGPL-3.0 (dynamisch gelinkt) |
| mpv (libmpv), FFmpeg | Videowiedergabe | GPL-2.0-or-later / LGPL-2.1-or-later |
| exiv2 | EXIF, IPTC, XMP | GPL-2.0-or-later |
| Tesseract, Leptonica | Texterkennung | Apache-2.0 / BSD-2-Clause |
| tessdata (Sprachdaten) | Texterkennung | Apache-2.0 |
| qpdf | PDFs teilen | Apache-2.0 |
| libheif, libde265, dav1d, aom | HEIC, AVIF | LGPL-3.0 / LGPL-3.0 / BSD-2-Clause / BSD-2-Clause |
| LibRaw | Kamera-RAW | LGPL-2.1 oder CDDL-1.0 |
| OpenJPEG | JPEG 2000 | BSD-2-Clause |
| DjVuLibre | DjVu | GPL-2.0-or-later |
| libjpeg-turbo | JPEG, verlustfreies Drehen | IJG, BSD-3-Clause, zlib |

Die Bibliotheken werden unverändert aus den Paketquellen übernommen:
Windows aus [MSYS2](https://packages.msys2.org) (UCRT64), macOS aus
[Homebrew](https://formulae.brew.sh). Den Quelltext genau dieser Versionen
stellen beide Projekte bereit (MSYS2: Quellpakete unter
https://repo.msys2.org/mingw/sources/, Homebrew: die in der jeweiligen Formel
verlinkten Quellarchive). Auf Anfrage über die Issues des Repos stellen wir den
Quelltext der mitgelieferten GPL- und LGPL-Bibliotheken auch selbst bereit.

Unter Linux (deb, rpm, Arch, Gentoo, Nix) bringt KiekAn! keine fremden
Bibliotheken mit; sie kommen aus der Distribution.
