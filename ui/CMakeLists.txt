cmake_minimum_required(VERSION 3.21)

project(ds4-translator-ui VERSION 0.1 LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)

# Same strictness the daemon builds with (see the top-level Makefile): this
# process talks to a privileged daemon over a socket, so silent conversions
# and format slips are compile errors here too. Applied per file rather than
# to the whole target on purpose -- moc/qmlcachegen output is generated code
# we do not control, and it does not survive -Werror.
set(DS4_UI_STRICT_WARNINGS
    -Wall -Wextra -Werror -Wshadow -Wformat=2 -Wformat-security
    -Wnull-dereference -Wpointer-arith -Wcast-align -Wundef -Wwrite-strings
)

# Widgets: QSystemTrayIcon and QMenu (the applet) live there; Qt Quick
# has no tray API.
find_package(Qt6 6.5 REQUIRED COMPONENTS Core Gui Quick QuickControls2 Widgets)

qt_standard_project_setup(REQUIRES 6.5)

# The .qml files live in qml/ rather than the module root; NEW makes Qt
# generate the extra qmldir files that layout needs.
qt_policy(SET QTP0004 NEW)

# The daemon's protocol client, shared verbatim with ds4-ctl (built from the
# top-level Makefile). Compiled into this binary rather than duplicated, so
# both front-ends speak to the daemon through one implementation.
set(DS4_SHARED_DIR ${CMAKE_CURRENT_SOURCE_DIR}/../src)

qt_add_executable(ds4-translator-ui
    src/main.cpp
    src/ipcclient.cpp
    src/ipcclient.h
    src/trayicon.cpp
    src/trayicon.h
    ${DS4_SHARED_DIR}/ipc-client.cpp
    ${DS4_SHARED_DIR}/ipc-client.h
)

qt_add_qml_module(ds4-translator-ui
    URI Ds4Translator
    VERSION 1.0
    QML_FILES
        qml/Main.qml
        qml/OptionSelector.qml
        qml/NameEditor.qml
    SOURCES
        src/daemoncontroller.h
        src/daemoncontroller.cpp
        src/appcontroller.h
        src/appcontroller.cpp
)

set_source_files_properties(
    src/main.cpp src/ipcclient.cpp src/daemoncontroller.cpp
    src/trayicon.cpp src/appcontroller.cpp
    ${DS4_SHARED_DIR}/ipc-client.cpp
    PROPERTIES COMPILE_OPTIONS "${DS4_UI_STRICT_WARNINGS}"
)

# src: the generated QML type registration includes our headers by bare name.
target_include_directories(ds4-translator-ui PRIVATE src ${DS4_SHARED_DIR})

target_link_libraries(ds4-translator-ui PRIVATE
    Qt6::Core
    Qt6::Gui
    Qt6::Quick
    Qt6::QuickControls2
    Qt6::Widgets
)

include(GNUInstallDirs)
install(TARGETS ds4-translator-ui RUNTIME DESTINATION ${CMAKE_INSTALL_BINDIR})
