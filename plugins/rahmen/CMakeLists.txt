# Kiek an – Beispiel-Plugin „Rahmen“
# SPDX-License-Identifier: GPL-3.0-or-later

qt_add_plugin(kiekan_rahmen SHARED)
target_sources(kiekan_rahmen PRIVATE RahmenPlugin.cpp rahmen.json)
target_include_directories(kiekan_rahmen PRIVATE ${PROJECT_SOURCE_DIR}/include)
target_link_libraries(kiekan_rahmen PRIVATE Qt6::Core Qt6::Gui)

# Im Build-Ordner neben das Programm legen, damit es ohne Installation gefunden wird.
set_target_properties(kiekan_rahmen PROPERTIES
    LIBRARY_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR}/plugins
    RUNTIME_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR}/plugins)

install(TARGETS kiekan_rahmen
    LIBRARY DESTINATION ${KIEKAN_INSTALL_PLUGINDIR}
    RUNTIME DESTINATION ${KIEKAN_INSTALL_PLUGINDIR})
