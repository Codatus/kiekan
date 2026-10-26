# Kiek an – zusätzliche Bildformate als Qt-Bildformat-Plugins
# SPDX-License-Identifier: GPL-3.0-or-later
#
# Jedes Format ist ein QImageIOPlugin. Dadurch funktioniert es überall, wo
# Kiek an Bilder liest: Anzeige, Vorladen, Kontaktabzug, Bearbeitung,
# Stapelkonvertierung, Texterkennung und KI-Schnittstelle.
# Kiek an lädt sie aus <Programmordner>/qtplugins/imageformats (Build) bzw.
# <prefix>/lib/kiekan/qtplugins/imageformats (installiert).

set(KIEKAN_QTPLUGIN_DIR "${CMAKE_BINARY_DIR}/qtplugins/imageformats")
set(KIEKAN_INSTALL_QTPLUGINDIR "${CMAKE_INSTALL_LIBDIR}/kiekan/qtplugins/imageformats")

function(kiekan_add_imageformat name)
    cmake_parse_arguments(ARG "" "" "SOURCES;LIBRARIES" ${ARGN})
    qt_add_plugin(kiekan_${name} SHARED CLASS_NAME ${name})
    target_sources(kiekan_${name} PRIVATE ${ARG_SOURCES})
    target_link_libraries(kiekan_${name} PRIVATE Qt6::Core Qt6::Gui ${ARG_LIBRARIES})
    set_target_properties(kiekan_${name} PROPERTIES
        LIBRARY_OUTPUT_DIRECTORY ${KIEKAN_QTPLUGIN_DIR}
        RUNTIME_OUTPUT_DIRECTORY ${KIEKAN_QTPLUGIN_DIR})
    if(NOT MSVC)
        target_compile_options(kiekan_${name} PRIVATE -Wall -Wextra)
    endif()
    install(TARGETS kiekan_${name}
        LIBRARY DESTINATION ${KIEKAN_INSTALL_QTPLUGINDIR}
        RUNTIME DESTINATION ${KIEKAN_INSTALL_QTPLUGINDIR})
    message(STATUS "Kiek an: Bildformat ${name}")
endfunction()

# PSD/PSB: eigener Leser, keine Bibliothek nötig
kiekan_add_imageformat(psd SOURCES psd/PsdPlugin.cpp psd/psd.json)

# HEIF, LIBRAW, OPENJP2, DJVU werden im Haupt-CMakeLists gesucht (auch das
# Programm selbst nutzt libheif/LibRaw für Metadaten).

if(HEIF_FOUND)
    kiekan_add_imageformat(heif SOURCES heif/HeifPlugin.cpp heif/heif.json LIBRARIES PkgConfig::HEIF)
endif()
if(LIBRAW_FOUND)
    kiekan_add_imageformat(raw SOURCES raw/RawPlugin.cpp raw/raw.json LIBRARIES PkgConfig::LIBRAW)
endif()
if(OPENJP2_FOUND)
    kiekan_add_imageformat(jp2 SOURCES jp2/Jp2Plugin.cpp jp2/jp2.json LIBRARIES PkgConfig::OPENJP2)
endif()
if(DJVU_FOUND)
    kiekan_add_imageformat(djvu SOURCES djvu/DjvuPlugin.cpp djvu/djvu.json LIBRARIES PkgConfig::DJVU)
endif()
