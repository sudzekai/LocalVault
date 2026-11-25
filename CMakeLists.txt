cmake_minimum_required(VERSION 3.21)
project(LocalVault VERSION 3.1.0 LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

find_package(Qt6 6.4 REQUIRED COMPONENTS Core Widgets Svg Network)
find_package(OpenSSL 3 REQUIRED COMPONENTS Crypto)
find_path(SODIUM_INCLUDE_DIR NAMES sodium.h REQUIRED)
find_library(SODIUM_LIBRARY NAMES sodium libsodium REQUIRED)
add_library(Sodium::Sodium UNKNOWN IMPORTED)
set_target_properties(Sodium::Sodium PROPERTIES
    IMPORTED_LOCATION "${SODIUM_LIBRARY}"
    INTERFACE_INCLUDE_DIRECTORIES "${SODIUM_INCLUDE_DIR}"
)
option(LOCALVAULT_SODIUM_STATIC "Link against a static libsodium library" OFF)
if(LOCALVAULT_SODIUM_STATIC)
    set_property(TARGET Sodium::Sodium APPEND PROPERTY INTERFACE_COMPILE_DEFINITIONS SODIUM_STATIC)
endif()

add_library(vault_core STATIC src/Vault.cpp src/RemoteFile.cpp)
target_include_directories(vault_core PUBLIC src)
target_link_libraries(vault_core PUBLIC Qt6::Core Sodium::Sodium PRIVATE OpenSSL::Crypto Qt6::Network Qt6::Gui)

add_library(vault_widgets STATIC src/MainWindow.cpp src/VaultActions.cpp src/Dialogs.cpp src/IconPicker.cpp src/TokenStore.cpp)
if(WIN32)
    target_link_libraries(vault_widgets PRIVATE crypt32)
endif()
target_link_libraries(vault_widgets PUBLIC vault_core Qt6::Widgets PRIVATE Qt6::Svg)
file(GLOB SITE_ICONS CONFIGURE_DEPENDS "assets/site-icons/*.svg")
qt_add_resources(vault_widgets site_icons PREFIX "/site-icons" BASE "assets/site-icons" FILES ${SITE_ICONS})
qt_add_resources(vault_widgets bootstrap_icons PREFIX "/actions" BASE "assets/bootstrap" FILES
    assets/bootstrap/plus-lg.svg assets/bootstrap/dash-lg.svg assets/bootstrap/trash.svg
    assets/bootstrap/pencil.svg assets/bootstrap/eye.svg assets/bootstrap/eye-slash.svg
    assets/bootstrap/box-arrow-up-right.svg assets/bootstrap/copy.svg assets/bootstrap/shuffle.svg
    assets/bootstrap/gear.svg assets/bootstrap/lock.svg assets/bootstrap/box-arrow-in-down.svg
    assets/bootstrap/box-arrow-up.svg assets/bootstrap/check-lg.svg assets/bootstrap/x-lg.svg
    assets/bootstrap/key.svg assets/bootstrap/globe2.svg
)
qt_add_executable(LocalVault WIN32 src/main.cpp)
target_link_libraries(LocalVault PRIVATE vault_widgets)
qt_add_resources(LocalVault application_icon
    PREFIX "/icons"
    BASE "assets"
    FILES "assets/localvault.png"
)
if(WIN32)
    enable_language(RC)
    target_sources(LocalVault PRIVATE assets/LocalVault.rc)
    target_include_directories(LocalVault PRIVATE assets)
elseif(CMAKE_SYSTEM_NAME STREQUAL "Linux")
    include(GNUInstallDirs)
    install(TARGETS LocalVault RUNTIME DESTINATION "${CMAKE_INSTALL_BINDIR}")
    install(FILES assets/localvault.desktop DESTINATION "${CMAKE_INSTALL_DATADIR}/applications")
    install(FILES assets/localvault.png DESTINATION "${CMAKE_INSTALL_DATADIR}/icons/hicolor/256x256/apps")
endif()
if(MSVC)
    target_compile_options(vault_core PUBLIC /utf-8)
endif()

include(CTest)
if(BUILD_TESTING)
    add_executable(vault_tests tests/VaultTests.cpp)
    target_link_libraries(vault_tests PRIVATE vault_core)
    add_test(NAME vault_tests COMMAND vault_tests)
    add_executable(remote_tests tests/RemoteTests.cpp)
    target_link_libraries(remote_tests PRIVATE vault_core)
    add_executable(widget_tests tests/WidgetTests.cpp)
    target_link_libraries(widget_tests PRIVATE vault_widgets)
    add_test(NAME widget_tests COMMAND widget_tests)
    set_tests_properties(widget_tests PROPERTIES ENVIRONMENT "QT_QPA_PLATFORM=offscreen" TIMEOUT 15)
endif()
