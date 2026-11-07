cmake_minimum_required(VERSION 3.16)
project(nordbuds CXX RC)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
if(NOT CMAKE_BUILD_TYPE)
    set(CMAKE_BUILD_TYPE Release)
endif()

add_compile_definitions(UNICODE _UNICODE WIN32_LEAN_AND_MEAN NOMINMAX _WIN32_WINNT=0x0A00 WINVER=0x0A00)
if(MSVC)
    add_compile_options(/W4 /utf-8 /permissive-)
else()
    add_compile_options(-Wall -Wextra -Wno-missing-field-initializers -Wno-cast-function-type)
    # One self-contained .exe: no libstdc++ / libgcc DLLs to ship.
    add_link_options(-static -municode)
    # MinGW's windres embeds the manifest from the .rc; MSVC's linker takes the .manifest source instead.
    set(CMAKE_RC_FLAGS "${CMAKE_RC_FLAGS} -DNORDBUDS_EMBED_MANIFEST")
endif()

set(RESOURCES res/nordbuds.rc)
if(MSVC)
    list(APPEND RESOURCES res/nordbuds.manifest)
endif()

add_executable(nordbuds WIN32
    src/main.cpp
    src/buds.cpp
    src/bluetooth.cpp
    src/protocol.cpp
    src/model.cpp
    src/window.cpp
    src/tray.cpp
    src/gfx.cpp
    src/settings.cpp
    src/util.cpp
    ${RESOURCES}
)
target_link_libraries(nordbuds PRIVATE ws2_32 bthprops d2d1 dwrite windowscodecs dwmapi ole32 uuid shell32 user32 gdi32 advapi32)

# Command line front end. Named nordbuds.com so "nordbuds status" in a terminal picks it before nordbuds.exe.
add_executable(nordbuds-cli src/cli.cpp src/util.cpp ${RESOURCES})
set_target_properties(nordbuds-cli PROPERTIES OUTPUT_NAME nordbuds SUFFIX .com)
target_link_libraries(nordbuds-cli PRIVATE user32 shell32)

install(TARGETS nordbuds nordbuds-cli DESTINATION .)

find_program(INNO_COMPILER iscc PATHS
    "$ENV{LOCALAPPDATA}/Programs/Inno Setup 6"
    "$ENV{ProgramFiles\(x86\)}/Inno Setup 6"
    "$ENV{ProgramFiles}/Inno Setup 6"
)
if(INNO_COMPILER)
    add_custom_target(installer
        COMMAND ${INNO_COMPILER} "${CMAKE_CURRENT_SOURCE_DIR}/installer.iss"
        DEPENDS nordbuds nordbuds-cli
        WORKING_DIRECTORY "${CMAKE_CURRENT_SOURCE_DIR}"
        COMMENT "Building Inno Setup installer executable"
    )
endif()
