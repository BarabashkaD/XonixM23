cmake_minimum_required(VERSION 3.25)
project(XonixM23 VERSION 0.1.0 LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 23)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

option(XONIX_SANITIZE "Build everything with AddressSanitizer (and UBSan where supported)" OFF)

if(XONIX_SANITIZE)
  if(WIN32)
    add_compile_options(-fsanitize=address)
    add_link_options(-fsanitize=address)
    execute_process(
      COMMAND ${CMAKE_CXX_COMPILER} -print-resource-dir
      OUTPUT_VARIABLE XONIX_CLANG_RESOURCE_DIR
      OUTPUT_STRIP_TRAILING_WHITESPACE)
    string(TOLOWER "${CMAKE_SYSTEM_PROCESSOR}" XONIX_PROCESSOR)
    if(XONIX_PROCESSOR MATCHES "^(amd64|x86_64|x64)$")
      set(XONIX_ASAN_ARCH x86_64)
    elseif(XONIX_PROCESSOR MATCHES "^(arm64|aarch64)$")
      set(XONIX_ASAN_ARCH aarch64)
    else()
      message(FATAL_ERROR "No known ASan runtime for processor '${CMAKE_SYSTEM_PROCESSOR}'")
    endif()
    set(XONIX_ASAN_DLL
        "${XONIX_CLANG_RESOURCE_DIR}/lib/windows/clang_rt.asan_dynamic-${XONIX_ASAN_ARCH}.dll")
    if(NOT EXISTS "${XONIX_ASAN_DLL}")
      message(FATAL_ERROR "ASan runtime not found: ${XONIX_ASAN_DLL}")
    endif()
  else()
    add_compile_options(-fsanitize=address,undefined -fno-sanitize-recover=all
                        -fno-omit-frame-pointer)
    add_link_options(-fsanitize=address,undefined)
  endif()
endif()

include(FetchContent)
set(FTXUI_BUILD_EXAMPLES OFF CACHE BOOL "" FORCE)
set(FTXUI_BUILD_DOCS OFF CACHE BOOL "" FORCE)
set(FTXUI_BUILD_TESTS OFF CACHE BOOL "" FORCE)
set(FTXUI_ENABLE_INSTALL OFF CACHE BOOL "" FORCE)
set(FTXUI_QUIET ON CACHE BOOL "" FORCE)
FetchContent_Declare(ftxui
  URL https://github.com/ArthurSonzogni/FTXUI/archive/refs/tags/v7.0.3.tar.gz
  URL_HASH SHA256=e7c62ffe19009759821b4f0f8df7f2a6fb83784c3a9f1477d81f56d3ee723c88
  SYSTEM)
FetchContent_MakeAvailable(ftxui)

# GNU-style flags for clang++ and g++ (also clang++ targeting the MSVC ABI); /W4 for cl and clang-cl.
if(MSVC AND NOT CMAKE_CXX_COMPILER_FRONTEND_VARIANT STREQUAL "GNU")
  set(XONIX_WARNINGS /W4)
else()
  set(XONIX_WARNINGS -Wall -Wextra -Wpedantic -Wconversion -Wsign-conversion -Wshadow)
endif()

add_library(xonix_core STATIC src/settings.cpp src/game.cpp)
target_include_directories(xonix_core PUBLIC src)
target_compile_options(xonix_core PRIVATE ${XONIX_WARNINGS})

add_executable(xonixm23 src/main.cpp)
target_link_libraries(xonixm23 PRIVATE xonix_core ftxui::component)
target_compile_options(xonixm23 PRIVATE ${XONIX_WARNINGS})

add_executable(xonix_tests tests/tests.cpp)
target_link_libraries(xonix_tests PRIVATE xonix_core)
target_compile_options(xonix_tests PRIVATE ${XONIX_WARNINGS})

if(XONIX_SANITIZE AND WIN32)
  foreach(tgt xonixm23 xonix_tests)
    add_custom_command(TARGET ${tgt} POST_BUILD
      COMMAND ${CMAKE_COMMAND} -E copy_if_different
              "${XONIX_ASAN_DLL}" "$<TARGET_FILE_DIR:${tgt}>")
  endforeach()
endif()

enable_testing()
add_test(NAME xonix_tests COMMAND xonix_tests)
