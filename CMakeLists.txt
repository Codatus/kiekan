# KiekAn! – schneller Bildbetrachter
# SPDX-License-Identifier: GPL-3.0-or-later

cmake_minimum_required(VERSION 3.16)

project(kiekan
    VERSION 0.10.0
    DESCRIPTION "Schneller, schlanker Bildbetrachter"
    LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_AUTOMOC ON)

if(NOT CMAKE_BUILD_TYPE AND NOT CMAKE_CONFIGURATION_TYPES)
    set(CMAKE_BUILD_TYPE Release CACHE STRING "Build type" FORCE)
endif()

option(KIEKAN_BUILD_EXAMPLE_PLUGIN "Beispiel-Plugin „Rahmen“ bauen" ON)
option(KIEKAN_WITH_VIDEO "Videowiedergabe über libmpv" ON)
option(KIEKAN_WITH_EXIF "EXIF/IPTC/XMP über exiv2" ON)
option(KIEKAN_WITH_PDF "PDF-Anzeige über QtPdf" ON)
option(KIEKAN_WITH_OCR "Texterkennung über Tesseract" ON)
option(KIEKAN_WITH_QPDF "PDFs teilen über qpdf" ON)

find_package(Qt6 6.2 REQUIRED COMPONENTS Core Gui Widgets Network OPTIONAL_COMPONENTS LinguistTools)
find_package(Threads REQUIRED)
find_package(PkgConfig)

# MSYS2: manche .pc-Dateien liefern MSYS-Pfade wie /ucrt64/include, die das
# native CMake nicht kennt. Nicht vorhandene Include-Pfade werden verworfen
# (der Compiler sucht dort ohnehin standardmäßig).
function(kiekan_fix_pkg_includes)
    if(NOT WIN32)
        return()
    endif()
    foreach(tgt IN LISTS ARGN)
        if(TARGET ${tgt})
            get_target_property(dirs ${tgt} INTERFACE_INCLUDE_DIRECTORIES)
            if(dirs)
                set(kept "")
                foreach(d IN LISTS dirs)
                    if(EXISTS "${d}")
                        list(APPEND kept "${d}")
                    endif()
                endforeach()
                set_target_properties(${tgt} PROPERTIES INTERFACE_INCLUDE_DIRECTORIES "${kept}")
            endif()
        endif()
    endforeach()
endfunction()

# Optionale Bausteine: fehlt einer, wird ohne die Funktion gebaut.
if(KIEKAN_WITH_PDF)
    find_package(Qt6 COMPONENTS Pdf QUIET)
endif()
if(KIEKAN_WITH_VIDEO)
    find_package(Qt6 COMPONENTS OpenGLWidgets QUIET)
    if(PkgConfig_FOUND)
        pkg_check_modules(MPV IMPORTED_TARGET mpv)
    endif()
endif()
if(KIEKAN_WITH_OCR AND PkgConfig_FOUND)
    pkg_check_modules(TESSERACT IMPORTED_TARGET tesseract)
endif()
if(KIEKAN_WITH_QPDF AND PkgConfig_FOUND)
    pkg_check_modules(QPDF IMPORTED_TARGET libqpdf)
endif()
if(KIEKAN_WITH_EXIF AND PkgConfig_FOUND)
    pkg_check_modules(EXIV2 IMPORTED_TARGET exiv2)
endif()
kiekan_fix_pkg_includes(PkgConfig::MPV PkgConfig::TESSERACT PkgConfig::QPDF PkgConfig::EXIV2)

include(GNUInstallDirs)
set(KIEKAN_INSTALL_PLUGINDIR "${CMAKE_INSTALL_LIBDIR}/kiekan/plugins")

qt_add_executable(kiekan WIN32
    src/main.cpp
    src/MainWindow.cpp
    src/MainWindow.h
    src/ImageView.cpp
    src/ImageView.h
    src/ImageLoader.cpp
    src/ImageLoader.h
    src/ImageOps.cpp
    src/ImageOps.h
    src/FilterRegistry.cpp
    src/FilterRegistry.h
    src/FilterDialog.cpp
    src/FilterDialog.h
    src/CurvesDialog.cpp
    src/CurvesDialog.h
    src/HistogramDialog.cpp
    src/HistogramDialog.h
    src/ResizeDialog.cpp
    src/ResizeDialog.h
    src/SettingsDialog.cpp
    src/SettingsDialog.h
    src/Metadata.cpp
    src/Metadata.h
    src/ExifDialog.cpp
    src/ExifDialog.h
    src/Icons.cpp
    src/Icons.h
    src/Media.cpp
    src/Media.h
    src/EditOps.cpp
    src/EditOps.h
    src/AiGuard.cpp
    src/AiGuard.h
    src/AiLogDialog.cpp
    src/AiLogDialog.h
    src/McpServer.cpp
    src/McpServer.h
    src/Ocr.cpp
    src/Ocr.h
    src/OcrDialog.cpp
    src/OcrDialog.h
    src/OcrWorker.cpp
    src/OcrWorker.h
    src/PdfSplit.cpp
    src/PdfSplit.h
    src/PdfSplitDialog.cpp
    src/PdfSplitDialog.h
    src/ViewerBridge.cpp
    src/ViewerBridge.h
    include/kiekan/FilterInterface.h
)

qt_add_resources(kiekan "icons"
    PREFIX "/"
    BASE "data"
    FILES data/kiekan.svg data/kiekan-klein.svg
)

# Übersetzungen: Quelltexte sind deutsch, weitere Sprachen als .ts-Dateien.
set(KIEKAN_TS_FILES translations/kiekan_en.ts)
if(TARGET Qt6::lrelease)
    qt_add_translation(KIEKAN_QM_FILES ${KIEKAN_TS_FILES})
else()
    # Manche Distributionen (z. B. Ubuntu 22.04) liefern lrelease ohne
    # CMake-Modul aus; dann das Programm direkt aufrufen.
    find_program(KIEKAN_LRELEASE NAMES lrelease-qt6 lrelease
        HINTS "${QT6_INSTALL_PREFIX}/${QT6_INSTALL_BINS}" "${QT6_INSTALL_PREFIX}/${QT6_INSTALL_LIBEXECS}"
              /usr/lib/qt6/bin
        REQUIRED)
    set(KIEKAN_QM_FILES)
    foreach(ts ${KIEKAN_TS_FILES})
        get_filename_component(base ${ts} NAME_WE)
        set(qm "${CMAKE_CURRENT_BINARY_DIR}/${base}.qm")
        add_custom_command(OUTPUT ${qm}
            COMMAND ${KIEKAN_LRELEASE} -silent ${CMAKE_CURRENT_SOURCE_DIR}/${ts} -qm ${qm}
            DEPENDS ${ts} VERBATIM)
        list(APPEND KIEKAN_QM_FILES ${qm})
    endforeach()
endif()
qt_add_resources(kiekan "translations"
    PREFIX "/i18n"
    BASE "${CMAKE_CURRENT_BINARY_DIR}"
    FILES ${KIEKAN_QM_FILES}
)

target_include_directories(kiekan PRIVATE include)

target_compile_definitions(kiekan PRIVATE
    KIEKAN_VERSION="${PROJECT_VERSION}"
    QT_NO_KEYWORDS
)
# Plugin-Ordner relativ zum Programm (bin/../lib/kiekan/plugins), damit er
# unabhängig vom Installationspräfix stimmt.
file(RELATIVE_PATH KIEKAN_PLUGIN_RELDIR
    "${CMAKE_INSTALL_PREFIX}/${CMAKE_INSTALL_BINDIR}"
    "${CMAKE_INSTALL_PREFIX}/${KIEKAN_INSTALL_PLUGINDIR}")
target_compile_definitions(kiekan PRIVATE KIEKAN_PLUGIN_RELDIR="${KIEKAN_PLUGIN_RELDIR}")
# Ebenso die eigenen Bildformat-Plugins; Debian/Ubuntu installieren nach
# lib/<Architektur>, nicht nach lib.
file(RELATIVE_PATH KIEKAN_QTPLUGIN_RELDIR
    "${CMAKE_INSTALL_PREFIX}/${CMAKE_INSTALL_BINDIR}"
    "${CMAKE_INSTALL_PREFIX}/${CMAKE_INSTALL_LIBDIR}/kiekan/qtplugins")
target_compile_definitions(kiekan PRIVATE KIEKAN_QTPLUGIN_RELDIR="${KIEKAN_QTPLUGIN_RELDIR}")

if(MSVC)
    target_compile_options(kiekan PRIVATE /W4)
else()
    target_compile_options(kiekan PRIVATE -Wall -Wextra)
endif()

target_link_libraries(kiekan PRIVATE Qt6::Core Qt6::Gui Qt6::Widgets Qt6::Network Threads::Threads)

if(TARGET Qt6::Pdf)
    target_link_libraries(kiekan PRIVATE Qt6::Pdf)
    target_compile_definitions(kiekan PRIVATE KIEKAN_HAVE_PDF)
    message(STATUS "KiekAn!: PDF-Anzeige aktiv")
endif()
if(MPV_FOUND AND TARGET Qt6::OpenGLWidgets)
    target_sources(kiekan PRIVATE src/VideoView.cpp src/VideoView.h)
    target_link_libraries(kiekan PRIVATE PkgConfig::MPV Qt6::OpenGLWidgets)
    target_compile_definitions(kiekan PRIVATE KIEKAN_HAVE_MPV)
    message(STATUS "KiekAn!: Videowiedergabe aktiv (libmpv ${MPV_VERSION})")
elseif(KIEKAN_WITH_VIDEO)
    message(WARNING "KiekAn!: ohne Videowiedergabe – libmpv-dev und Qt6 OpenGLWidgets "
                    "(Debian/Ubuntu: libqt6opengl6-dev) werden gebraucht")
endif()
if(TESSERACT_FOUND)
    target_link_libraries(kiekan PRIVATE PkgConfig::TESSERACT)
    target_compile_definitions(kiekan PRIVATE KIEKAN_HAVE_TESSERACT)
    message(STATUS "KiekAn!: Texterkennung aktiv (Tesseract ${TESSERACT_VERSION})")
endif()
if(QPDF_FOUND)
    target_link_libraries(kiekan PRIVATE PkgConfig::QPDF)
    target_compile_definitions(kiekan PRIVATE KIEKAN_HAVE_QPDF)
    message(STATUS "KiekAn!: PDF teilen aktiv (qpdf ${QPDF_VERSION})")
endif()
if(EXIV2_FOUND)
    target_link_libraries(kiekan PRIVATE PkgConfig::EXIV2)
    target_compile_definitions(kiekan PRIVATE KIEKAN_HAVE_EXIV2)
    message(STATUS "KiekAn!: EXIF aktiv (exiv2 ${EXIV2_VERSION})")
endif()

if(KIEKAN_BUILD_EXAMPLE_PLUGIN)
    add_subdirectory(plugins/rahmen)
endif()
if(PkgConfig_FOUND)
    pkg_check_modules(HEIF IMPORTED_TARGET GLOBAL libheif)
    pkg_check_modules(LIBRAW IMPORTED_TARGET GLOBAL libraw_r)
    pkg_check_modules(OPENJP2 IMPORTED_TARGET GLOBAL libopenjp2)
    pkg_check_modules(DJVU IMPORTED_TARGET GLOBAL ddjvuapi)
    kiekan_fix_pkg_includes(PkgConfig::HEIF PkgConfig::LIBRAW PkgConfig::OPENJP2 PkgConfig::DJVU)
endif()
# Metadaten aus HEIC/AVIF (EXIF-Block über libheif) und RAW (LibRaw), wo
# exiv2 diese Formate nicht kennt (z. B. 0.27 ohne BMFF-Unterstützung).
if(HEIF_FOUND AND EXIV2_FOUND)
    target_link_libraries(kiekan PRIVATE PkgConfig::HEIF)
    target_compile_definitions(kiekan PRIVATE KIEKAN_HAVE_HEIF)
endif()
if(LIBRAW_FOUND)
    target_link_libraries(kiekan PRIVATE PkgConfig::LIBRAW)
    target_compile_definitions(kiekan PRIVATE KIEKAN_HAVE_LIBRAW)
endif()
add_subdirectory(plugins/imageformats)

# --- Tests -------------------------------------------------------------------

include(CTest)
if(BUILD_TESTING)
    qt_add_executable(filtertest tests/filtertest.cpp
        src/FilterRegistry.cpp src/ImageOps.cpp)
    target_include_directories(filtertest PRIVATE include src)
    target_compile_definitions(filtertest PRIVATE KIEKAN_PLUGIN_RELDIR="${KIEKAN_PLUGIN_RELDIR}")
    target_link_libraries(filtertest PRIVATE Qt6::Core Qt6::Gui Threads::Threads)
    add_test(NAME filter COMMAND filtertest)

    qt_add_executable(formattest tests/formattest.cpp)
    target_link_libraries(formattest PRIVATE Qt6::Core Qt6::Gui)
    set_tests_properties(filter PROPERTIES ENVIRONMENT "QT_QPA_PLATFORM=offscreen")
endif()

# --- Installation ------------------------------------------------------------

install(TARGETS kiekan RUNTIME DESTINATION ${CMAKE_INSTALL_BINDIR})
install(FILES include/kiekan/FilterInterface.h
        DESTINATION ${CMAKE_INSTALL_INCLUDEDIR}/kiekan)

if(UNIX AND NOT APPLE)
    install(FILES data/kiekan.desktop
            DESTINATION ${CMAKE_INSTALL_DATADIR}/applications)
    install(FILES data/io.github.codatus.kiekan.metainfo.xml
            DESTINATION ${CMAKE_INSTALL_DATADIR}/metainfo)
    install(FILES data/kiekan.svg
            DESTINATION ${CMAKE_INSTALL_DATADIR}/icons/hicolor/scalable/apps)
    # Vereinfachter Fisch für kleine Größen (ohne Kiemen und Flosse)
    foreach(size 16 22 24 32)
        install(FILES data/kiekan-klein.svg
                DESTINATION ${CMAKE_INSTALL_DATADIR}/icons/hicolor/${size}x${size}/apps
                RENAME kiekan.svg)
    endforeach()
    install(FILES LICENSE README.md
            DESTINATION ${CMAKE_INSTALL_DOCDIR})
endif()

# --- Pakete (cpack -G DEB / cpack -G RPM) ------------------------------------

set(CPACK_PACKAGE_NAME "kiekan")
set(CPACK_PACKAGE_VENDOR "KiekAn!")
set(CPACK_PACKAGE_CONTACT "Carl Kliefert <228213888+Codatus@users.noreply.github.com>")
set(CPACK_PACKAGE_DESCRIPTION_SUMMARY "KiekAn! - schneller, schlanker Bildbetrachter")
set(CPACK_RESOURCE_FILE_LICENSE "${CMAKE_CURRENT_SOURCE_DIR}/LICENSE")
set(CPACK_PACKAGING_INSTALL_PREFIX "/usr")
# Pakete hängen an den Bibliotheksversionen ihrer Distribution; mit
# -DKIEKAN_PKG_SUFFIX=ubuntu24.04 bzw. fedora43 steht das im Dateinamen.
set(KIEKAN_PKG_SUFFIX "" CACHE STRING "Distribution im Paketnamen (z. B. ubuntu24.04, fedora43)")
if(KIEKAN_PKG_SUFFIX)
    set(CPACK_DEBIAN_FILE_NAME "kiekan_${PROJECT_VERSION}_${KIEKAN_PKG_SUFFIX}_amd64.deb")
    set(CPACK_RPM_FILE_NAME "kiekan-${PROJECT_VERSION}-1.${KIEKAN_PKG_SUFFIX}.x86_64.rpm")
else()
    set(CPACK_DEBIAN_FILE_NAME DEB-DEFAULT)
    set(CPACK_RPM_FILE_NAME RPM-DEFAULT)
endif()
set(CPACK_DEBIAN_PACKAGE_SECTION "graphics")
set(CPACK_DEBIAN_PACKAGE_SHLIBDEPS ON)
# Bildformat-Plugins und Qt-Übersetzungen werden zur Laufzeit geladen und
# tauchen daher nicht in den automatisch ermittelten Abhängigkeiten auf.
# Ab Qt 6.6 (z. B. Debian 13) liegen SVG-Bild- und Symbol-Plugins in qt6-svg-plugins,
# ab libheif 1.16 die Decoder für HEIC (libde265) und AVIF (dav1d/aom) in eigenen Paketen.
set(CPACK_DEBIAN_PACKAGE_DEPENDS "qt6-qpa-plugins, qt6-image-formats-plugins, libqt6svg6, qt6-svg-plugins | libqt6svg6 (<< 6.6), libheif-plugin-libde265 | libheif1 (<< 1.16), libheif-plugin-dav1d | libheif-plugin-aomdec | libheif1 (<< 1.16)")
set(CPACK_DEBIAN_PACKAGE_RECOMMENDS "qt6-translations-l10n, tesseract-ocr-deu, tesseract-ocr-eng, tesseract-ocr-osd")
set(CPACK_DEBIAN_PACKAGE_DESCRIPTION
"KiekAn! ist ein schneller Bildbetrachter im Stil von IrfanView.
 Er zeigt Bilder sofort an, blaettert per Tastatur durch Ordner und bietet
 Bearbeitung: Drehen, Zuschneiden, Groesse aendern, Farben, Gradationskurven,
 Histogramm, Schaerfen, Effekte und Filter-Plugins.")

# RPM: Bibliotheken ermittelt rpmbuild selbst; Laufzeit-Plugins heißen bei
# Fedora und openSUSE verschieden.
set(KIEKAN_RPM_FAMILY "" CACHE STRING "fedora oder suse (leer: aus /etc/os-release)")
if(NOT KIEKAN_RPM_FAMILY AND EXISTS /etc/os-release)
    file(STRINGS /etc/os-release _os_like REGEX "^ID(_LIKE)?=")
    if(_os_like MATCHES "suse")
        set(KIEKAN_RPM_FAMILY suse)
    else()
        set(KIEKAN_RPM_FAMILY fedora)
    endif()
endif()
set(CPACK_RPM_PACKAGE_LICENSE "GPL-3.0-or-later")
set(CPACK_RPM_PACKAGE_GROUP "Productivity/Graphics/Viewers")
set(CPACK_RPM_PACKAGE_URL "https://github.com/Codatus/kiekan")
set(CPACK_RPM_PACKAGE_DESCRIPTION "${CPACK_DEBIAN_PACKAGE_DESCRIPTION}")
set(CPACK_RPM_PACKAGE_AUTOREQ ON)
set(CPACK_RPM_EXCLUDE_FROM_AUTO_FILELIST_ADDITION
    /usr/share/applications /usr/share/metainfo /usr/share/icons /usr/share/icons/hicolor
    /usr/share/icons/hicolor/scalable /usr/share/icons/hicolor/scalable/apps
    /usr/share/icons/hicolor/16x16 /usr/share/icons/hicolor/16x16/apps
    /usr/share/icons/hicolor/22x22 /usr/share/icons/hicolor/22x22/apps
    /usr/share/icons/hicolor/24x24 /usr/share/icons/hicolor/24x24/apps
    /usr/share/icons/hicolor/32x32 /usr/share/icons/hicolor/32x32/apps
    /usr/include /usr/lib64 /usr/share/doc)
if(KIEKAN_RPM_FAMILY STREQUAL "suse")
    set(CPACK_RPM_PACKAGE_REQUIRES "qt6-imageformats, libQt6Svg6, libheif-dav1d")
    set(KIEKAN_RPM_RECOMMENDS libheif-ffmpeg tesseract-ocr-traineddata-deu tesseract-ocr-traineddata-eng tesseract-ocr-traineddata-osd qt6-translations)
else()
    # HEIC braucht bei Fedora libheif-freeworld aus RPM Fusion (Patentgründe)
    set(CPACK_RPM_PACKAGE_REQUIRES "qt6-qtimageformats, qt6-qtsvg")
    set(KIEKAN_RPM_RECOMMENDS libheif-freeworld tesseract-langpack-deu tesseract-langpack-eng tesseract-osd qt6-qttranslations)
endif()
# CPack kennt kein Recommends; die Zeilen landen über den Spec-Kopf im Paket.
set(_rec "")
foreach(_p ${KIEKAN_RPM_RECOMMENDS})
    string(APPEND _rec "\nRecommends: ${_p}")
endforeach()
set(CPACK_RPM_SPEC_MORE_DEFINE "%global kiekan_rpm 1${_rec}")
include(CPack)
