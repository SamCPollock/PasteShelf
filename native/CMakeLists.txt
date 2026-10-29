cmake_minimum_required(VERSION 3.24)
project(PasteShelfNative VERSION 0.1.0 LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_AUTOMOC ON)
set(CMAKE_INTERPROCEDURAL_OPTIMIZATION_RELEASE ON)

find_package(Qt6 6.8 REQUIRED COMPONENTS Core Gui Widgets Network)

qt_add_executable(pasteshelf-native
    src/main.cpp
)

target_link_libraries(pasteshelf-native PRIVATE
    Qt6::Core
    Qt6::Gui
    Qt6::Widgets
    Qt6::Network
)

target_compile_options(pasteshelf-native PRIVATE
    $<$<CONFIG:Release>:-Os>
)

enable_testing()
add_test(NAME native-storage-and-mime
    COMMAND pasteshelf-native --self-test
)
add_test(NAME native-animation-lifecycle
    COMMAND pasteshelf-native --no-single-instance --no-kwin --animation-stress-test
)
add_test(NAME native-hover-open-delay
    COMMAND pasteshelf-native --no-single-instance --no-kwin --hover-open-delay-test
)
add_test(NAME native-hover-hold
    COMMAND pasteshelf-native --no-single-instance --no-kwin --hover-hold-test
)
add_test(NAME native-hover-leave
    COMMAND pasteshelf-native --no-single-instance --no-kwin --hover-leave-test
)
add_test(NAME native-anchor-animation
    COMMAND pasteshelf-native --no-single-instance --no-kwin --anchor-animation-test
)
set_tests_properties(native-storage-and-mime native-animation-lifecycle native-hover-open-delay native-hover-hold native-hover-leave native-anchor-animation PROPERTIES
    ENVIRONMENT "QT_QPA_PLATFORM=offscreen;XDG_CONFIG_HOME=${CMAKE_CURRENT_BINARY_DIR}/test-xdg/config;XDG_DATA_HOME=${CMAKE_CURRENT_BINARY_DIR}/test-xdg/data;XDG_CACHE_HOME=${CMAKE_CURRENT_BINARY_DIR}/test-xdg/cache"
    TIMEOUT 15
)

install(TARGETS pasteshelf-native RUNTIME DESTINATION bin)
