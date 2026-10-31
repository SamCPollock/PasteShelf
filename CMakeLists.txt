cmake_minimum_required(VERSION 3.24)

if(APPLE AND NOT CMAKE_OSX_DEPLOYMENT_TARGET)
    set(CMAKE_OSX_DEPLOYMENT_TARGET "12.0" CACHE STRING
        "Minimum supported macOS version")
endif()

project(PastaShelf VERSION 0.1.3 LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_AUTOMOC ON)
set(CMAKE_INTERPROCEDURAL_OPTIMIZATION_RELEASE ON)

find_package(Qt6 6.8 REQUIRED COMPONENTS Core Gui Widgets Network)

qt_add_executable(pastashelf
    src/main.cpp
)

qt_add_resources(pastashelf "pastashelf_resources"
    PREFIX "/"
    FILES
        assets/io.github.pastashelf.PastaShelf.png
)

if(APPLE)
    set(PASTASHELF_MACOS_ICON "${CMAKE_CURRENT_SOURCE_DIR}/assets/PastaShelf.icns")
    set_source_files_properties("${PASTASHELF_MACOS_ICON}" PROPERTIES
        MACOSX_PACKAGE_LOCATION "Resources"
    )
    target_sources(pastashelf PRIVATE "${PASTASHELF_MACOS_ICON}")
    set_target_properties(pastashelf PROPERTIES
        MACOSX_BUNDLE TRUE
        MACOSX_BUNDLE_BUNDLE_NAME "PastaShelf"
        MACOSX_BUNDLE_GUI_IDENTIFIER "io.github.pastashelf.PastaShelf"
        MACOSX_BUNDLE_BUNDLE_VERSION "${PROJECT_VERSION}"
        MACOSX_BUNDLE_SHORT_VERSION_STRING "${PROJECT_VERSION}"
        MACOSX_BUNDLE_ICON_FILE "PastaShelf.icns"
        OUTPUT_NAME "PastaShelf"
    )
endif()

target_link_libraries(pastashelf PRIVATE
    Qt6::Core
    Qt6::Gui
    Qt6::Widgets
    Qt6::Network
)

target_compile_options(pastashelf PRIVATE
    $<$<CONFIG:Release>:-Os>
)

enable_testing()
add_test(NAME storage-and-mime
    COMMAND pastashelf --self-test
)
add_test(NAME animation-lifecycle
    COMMAND pastashelf --no-single-instance --no-kwin --no-onboarding --animation-stress-test
)
add_test(NAME hover-open-delay
    COMMAND pastashelf --no-single-instance --no-kwin --no-onboarding --hover-open-delay-test
)
add_test(NAME hover-hold
    COMMAND pastashelf --no-single-instance --no-kwin --no-onboarding --hover-hold-test
)
add_test(NAME hover-leave
    COMMAND pastashelf --no-single-instance --no-kwin --no-onboarding --hover-leave-test
)
add_test(NAME anchor-animation
    COMMAND pastashelf --no-single-instance --no-kwin --no-onboarding --anchor-animation-test
)
set_tests_properties(storage-and-mime animation-lifecycle hover-open-delay hover-hold hover-leave anchor-animation PROPERTIES
    ENVIRONMENT "QT_QPA_PLATFORM=offscreen;XDG_CONFIG_HOME=${CMAKE_CURRENT_BINARY_DIR}/test-xdg/config;XDG_DATA_HOME=${CMAKE_CURRENT_BINARY_DIR}/test-xdg/data;XDG_CACHE_HOME=${CMAKE_CURRENT_BINARY_DIR}/test-xdg/cache"
    TIMEOUT 15
)

install(TARGETS pastashelf
    BUNDLE DESTINATION .
    RUNTIME DESTINATION bin
)
