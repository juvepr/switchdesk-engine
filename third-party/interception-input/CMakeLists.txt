cmake_minimum_required(VERSION 3.25)

project(
    interception_input
    VERSION 0.1.0
    LANGUAGES C CXX
)

set(CMAKE_C_STANDARD 11)
set(CMAKE_C_STANDARD_REQUIRED ON)
set(CMAKE_C_EXTENSIONS OFF)

set(CMAKE_CXX_STANDARD 23)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

# Consumers embedding this library via add_subdirectory() get the library
# target only; examples and tests are a standalone-build concern.
option(INTERCEPTION_INPUT_BUILD_EXAMPLES
    "Build the interception-input smoke-test executables"
    ${PROJECT_IS_TOP_LEVEL}
)

option(INTERCEPTION_INPUT_BUILD_TESTS
    "Build and register the interception-input unit tests"
    ${PROJECT_IS_TOP_LEVEL}
)

add_library(interception_input STATIC
    src/input.cpp
    src/interception_context.cpp
    src/device_selector.cpp
    external/interception/interception.c
)

target_compile_definitions(interception_input
    PRIVATE
        INTERCEPTION_STATIC
)

target_include_directories(interception_input
    PUBLIC
        $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>

    PRIVATE
        ${CMAKE_CURRENT_SOURCE_DIR}/src
        ${CMAKE_CURRENT_SOURCE_DIR}/external/interception
)

target_compile_features(interception_input
    PUBLIC
        cxx_std_23
)

set_target_properties(interception_input PROPERTIES
    OUTPUT_NAME "interception_input"
)

# Warnings for first-party C++ sources only; vendored C is compiled as-is.
set(INTERCEPTION_INPUT_CXX_WARNINGS
    "$<$<AND:$<COMPILE_LANGUAGE:CXX>,$<CXX_COMPILER_ID:GNU,Clang>>:-Wall;-Wextra>"
)

target_compile_options(interception_input
    PRIVATE
        ${INTERCEPTION_INPUT_CXX_WARNINGS}
)

if(INTERCEPTION_INPUT_BUILD_EXAMPLES)
    add_executable(keyboard_smoke examples/keyboard_smoke.cpp)
    add_executable(mouse_smoke examples/mouse_smoke.cpp)
    add_executable(release_all_smoke examples/release_all_smoke.cpp)

    foreach(example_target IN ITEMS keyboard_smoke mouse_smoke release_all_smoke)
        target_link_libraries(${example_target} PRIVATE interception_input)
        target_compile_options(${example_target} PRIVATE ${INTERCEPTION_INPUT_CXX_WARNINGS})
    endforeach()
endif()

if(INTERCEPTION_INPUT_BUILD_TESTS)
    enable_testing()

    add_executable(state_tests tests/state_tests.cpp)

    target_link_libraries(state_tests PRIVATE interception_input)

    target_include_directories(state_tests
        PRIVATE
            ${CMAKE_CURRENT_SOURCE_DIR}/src
    )

    target_compile_options(state_tests PRIVATE ${INTERCEPTION_INPUT_CXX_WARNINGS})

    add_test(NAME state_tests COMMAND state_tests)
endif()
