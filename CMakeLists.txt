cmake_minimum_required(VERSION 3.16)
project(SoftDipsManager VERSION 0.1.1 LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

if(NOT CMAKE_BUILD_TYPE AND NOT CMAKE_CONFIGURATION_TYPES)
    set(CMAKE_BUILD_TYPE Release CACHE STRING "" FORCE)
endif()

# Qt: prefer Qt6, fall back to Qt5 (helps arm64 / newer CI images).
find_package(QT NAMES Qt6 Qt5 REQUIRED COMPONENTS Widgets)
find_package(Qt${QT_VERSION_MAJOR} REQUIRED COMPONENTS Widgets)

# ── Core library (no Qt dependency — keeps tests portable) ──
add_library(softdips_core STATIC
    src/core/softdips.cpp
    src/core/softdips.h
)
target_include_directories(softdips_core PUBLIC src/core)

# ── CLI executable ──
add_executable(softdips_cli src/cli/main_cli.cpp)
target_link_libraries(softdips_cli PRIVATE softdips_core)

# ── GUI executable ──
set(CMAKE_AUTOMOC ON)
set(CMAKE_AUTORCC ON)
set(CMAKE_AUTOUIC ON)

add_executable(softdips_gui
    src/gui/main_gui.cpp
    src/gui/mainwindow.h
    src/gui/mainwindow.cpp
    src/gui/settingsdialog.h
    src/gui/settingsdialog.cpp
)
target_link_libraries(softdips_gui PRIVATE softdips_core Qt${QT_VERSION_MAJOR}::Widgets)
if(WIN32)
    set_target_properties(softdips_gui PROPERTIES WIN32_EXECUTABLE ON)
elseif(APPLE)
    set_target_properties(softdips_gui PROPERTIES MACOSX_BUNDLE ON)
endif()

# ── Tests (CTest) ──
enable_testing()
add_executable(softdips_tests tests/test_core.cpp)
target_link_libraries(softdips_tests PRIVATE softdips_core)
add_test(NAME core_tests COMMAND softdips_tests)

# ── Install ──
install(TARGETS softdips_cli softdips_gui
    RUNTIME DESTINATION bin
    BUNDLE DESTINATION .
)
