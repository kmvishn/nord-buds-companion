cmake_minimum_required(VERSION 3.16)
project(nordbuds CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
if(NOT CMAKE_BUILD_TYPE)
    set(CMAKE_BUILD_TYPE RelWithDebInfo)
endif()

find_package(PkgConfig REQUIRED)
pkg_check_modules(GTK REQUIRED IMPORTED_TARGET gtk4)

add_executable(nordbuds
    src/main.cpp
    src/bt.cpp
    src/protocol.cpp
    src/model.cpp
    src/buds.cpp
    src/indicator.cpp
    src/window.cpp
    src/settings.cpp
    src/api.cpp
)
target_compile_options(nordbuds PRIVATE -Wall -Wextra -Wno-missing-field-initializers)
target_link_libraries(nordbuds PRIVATE PkgConfig::GTK)
set(NORDBUDS_ICON_DIR "${CMAKE_INSTALL_PREFIX}/share/nordbuds/icons")
# GTK only recolours symbolic icons found inside a theme, so the window uses the hicolor copy.
set(NORDBUDS_ICON_ROOT "${CMAKE_INSTALL_PREFIX}/share/icons")
target_compile_definitions(nordbuds PRIVATE NORDBUDS_ICON_DIR="${NORDBUDS_ICON_DIR}"
                                            NORDBUDS_ICON_ROOT="${NORDBUDS_ICON_ROOT}")

set(NORDBUDS_BIN "${CMAKE_INSTALL_PREFIX}/bin/nordbuds")
configure_file(data/com.nordbuds.Companion.desktop.in com.nordbuds.Companion.desktop @ONLY)
configure_file(data/nordbuds-autostart.desktop.in nordbuds-autostart.desktop @ONLY)

option(NORDBUDS_AUTOSTART "Start the background service at login" ON)
option(NORDBUDS_GNOME_EXTENSION "Install the GNOME Shell top-bar extension" ON)
set(NORDBUDS_EXTENSION_UUID "nordbuds@nordbuds.local")

install(TARGETS nordbuds DESTINATION bin)
install(FILES ${CMAKE_BINARY_DIR}/com.nordbuds.Companion.desktop DESTINATION share/applications)
install(FILES data/icons/nordbuds-symbolic.svg DESTINATION share/nordbuds/icons)
install(FILES data/icons/nordbuds-symbolic.svg DESTINATION share/icons/hicolor/scalable/apps)
if(NORDBUDS_AUTOSTART)
    install(FILES ${CMAKE_BINARY_DIR}/nordbuds-autostart.desktop
            DESTINATION "$ENV{HOME}/.config/autostart")
endif()
if(NORDBUDS_GNOME_EXTENSION)
    install(DIRECTORY gnome-extension/
            DESTINATION "$ENV{HOME}/.local/share/gnome-shell/extensions/${NORDBUDS_EXTENSION_UUID}")
    install(FILES data/icons/nordbuds-symbolic.svg
            DESTINATION "$ENV{HOME}/.local/share/gnome-shell/extensions/${NORDBUDS_EXTENSION_UUID}/icons")
endif()
