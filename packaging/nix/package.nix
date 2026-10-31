# Nix-Paket für KiekAn! (NixOS und Nix auf anderen Systemen)
# Ausprobieren im Quellordner:  nix-build packaging/nix && ./result/bin/kiekan
{
  lib,
  stdenv,
  cmake,
  ninja,
  pkg-config,
  qt6,
  mpv-unwrapped,
  exiv2,
  tesseract,
  leptonica,
  qpdf,
  libheif,
  libraw,
  openjpeg,
  djvulibre,
  libjpeg_turbo,
  libarchive,
  curl,
}:

let
  # Sprachdaten: Deutsch, Englisch und Seitenlage
  tesseractLangs = tesseract.override { enableLanguages = [ "deu" "eng" "osd" ]; };
in
stdenv.mkDerivation {
  pname = "kiekan";
  version = "0.13.0";
  # Quelltext des Repos ohne Build-Ordner
  src = lib.cleanSourceWith {
    src = ../..;
    filter = path: type: !(lib.hasPrefix "build" (baseNameOf path)) && baseNameOf path != "result";
  };

  nativeBuildInputs = [ cmake ninja pkg-config qt6.wrapQtAppsHook qt6.qttools ];
  buildInputs = [
    qt6.qtbase
    qt6.qtsvg
    qt6.qtimageformats
    qt6.qtwebengine   # bringt QtPdf mit
    mpv-unwrapped
    exiv2
    tesseractLangs
    leptonica
    qpdf
    libheif
    libraw
    openjpeg
    djvulibre
    libjpeg_turbo
    # tesseract.pc verlangt beide beim Linken
    libarchive
    curl
  ];

  doCheck = true;
  preCheck = "export QT_QPA_PLATFORM=offscreen";

  meta = {
    description = "Fast, lightweight image viewer with editing, OCR and an MCP interface for AI assistants";
    homepage = "https://github.com/Codatus/kiekan";
    license = lib.licenses.gpl3Plus;
    mainProgram = "kiekan";
    platforms = lib.platforms.linux;
  };
}
