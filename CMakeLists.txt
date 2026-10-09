cmake_minimum_required(VERSION 3.20)
project(sopt VERSION 0.6.10 LANGUAGES C CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

if(NOT CMAKE_BUILD_TYPE AND NOT CMAKE_CONFIGURATION_TYPES)
  set(CMAKE_BUILD_TYPE Release CACHE STRING "" FORCE)
endif()

option(SOPT_BUILD_TESTS "Build unit tests" ON)
option(SOPT_BUILD_BENCH "Build optimizer benchmark" ON)

# CPU evaluation is the reference for GPU float32 results, so the tool itself
# must not contract a*b+c into FMA or use fast-math.
function(sopt_fp_flags target)
  if(MSVC)
    target_compile_options(${target} PRIVATE /fp:precise /W4 /permissive- /utf-8)
  else()
    target_compile_options(${target} PRIVATE -ffp-contract=off -fno-fast-math -Wall -Wextra)
    if(CMAKE_SIZEOF_VOID_P EQUAL 4)
      target_compile_options(${target} PRIVATE -msse2 -mfpmath=sse)
    endif()
  endif()
endfunction()

find_package(Threads REQUIRED)

add_library(sopt-ir
  src/ir/ops.cpp
  src/ir/eval.cpp
  src/ir/expr.cpp
  src/ir/parser.cpp)
target_include_directories(sopt-ir PUBLIC src)
sopt_fp_flags(sopt-ir)

add_library(sopt-verify
  src/verify/points.cpp
  src/verify/verify.cpp
  src/verify/exact.cpp
  src/verify/problems.cpp
  src/verify/bound.cpp)
target_link_libraries(sopt-verify PUBLIC sopt-ir Threads::Threads)
sopt_fp_flags(sopt-verify)

# Zstandard (third_party/zstd, BSD): compression for the disk-backed search bank.
add_library(sopt-zstd STATIC third_party/zstd/zstd.c)
target_include_directories(sopt-zstd PUBLIC third_party/zstd)
target_compile_definitions(sopt-zstd PRIVATE ZSTD_DISABLE_ASM ZSTD_NO_TRACE)
set_target_properties(sopt-zstd PROPERTIES POSITION_INDEPENDENT_CODE ON)

add_library(sopt-search
  src/search/diskstore.cpp
  src/search/enumerator.cpp
  src/search/driver.cpp
  src/search/generalize.cpp
  src/search/subtrees.cpp
  src/search/cuts.cpp
  src/search/library.cpp
  src/search/reshape.cpp
  ${CMAKE_CURRENT_BINARY_DIR}/library_builtin.cpp)
# The rewrite library is built in (used when library/rewrites.txt is not found at run time).
add_custom_command(OUTPUT ${CMAKE_CURRENT_BINARY_DIR}/library_builtin.cpp
  COMMAND ${CMAKE_COMMAND} -DIN=${CMAKE_CURRENT_SOURCE_DIR}/library/rewrites.txt
          -DOUT=${CMAKE_CURRENT_BINARY_DIR}/library_builtin.cpp -P ${CMAKE_CURRENT_SOURCE_DIR}/cmake/embed_library.cmake
  DEPENDS ${CMAKE_CURRENT_SOURCE_DIR}/library/rewrites.txt ${CMAKE_CURRENT_SOURCE_DIR}/cmake/embed_library.cmake
  VERBATIM)
# GPU family shares from the Steam survey (tools/site/steam_share.py), for easy mode's picks.
add_custom_command(OUTPUT ${CMAKE_CURRENT_BINARY_DIR}/steam_share_builtin.cpp
  COMMAND ${CMAKE_COMMAND} -DIN=${CMAKE_CURRENT_SOURCE_DIR}/data/steam-gpu-share.txt
          -DOUT=${CMAKE_CURRENT_BINARY_DIR}/steam_share_builtin.cpp -DVAR=kSteamGpuShare
          -P ${CMAKE_CURRENT_SOURCE_DIR}/cmake/embed_library.cmake
  DEPENDS ${CMAKE_CURRENT_SOURCE_DIR}/data/steam-gpu-share.txt ${CMAKE_CURRENT_SOURCE_DIR}/cmake/embed_library.cmake
  VERBATIM)
target_link_libraries(sopt-search PUBLIC sopt-verify PRIVATE sopt-zstd)
sopt_fp_flags(sopt-search)

add_library(sopt-measure
  src/measure/tools.cpp
  src/measure/isa.cpp
  src/measure/sass.cpp
  src/measure/backends.cpp
  src/measure/driverstats.cpp)
target_link_libraries(sopt-measure PUBLIC sopt-ir Threads::Threads)
sopt_fp_flags(sopt-measure)

# ReShade FX front end (vendored from ReShade, BSD-3), used as the parser for sopt-fx.
add_library(reshadefx STATIC
  third_party/reshadefx/effect_expression.cpp
  third_party/reshadefx/effect_lexer.cpp
  third_party/reshadefx/effect_parser_exp.cpp
  third_party/reshadefx/effect_parser_stmt.cpp
  third_party/reshadefx/effect_preprocessor.cpp
  third_party/reshadefx/effect_symbol_table.cpp)
target_include_directories(reshadefx PUBLIC third_party/reshadefx)
set_target_properties(reshadefx PROPERTIES CXX_STANDARD 17)
if(MSVC)
  target_compile_options(reshadefx PRIVATE /fp:precise /utf-8)
else()
  target_compile_options(reshadefx PRIVATE -ffp-contract=off -fno-fast-math)
endif()

add_library(sopt-fx-lib
  src/fx/codegen.cpp
  src/fx/frontend.cpp
  src/fx/variants.cpp
  src/fx/platforms.cpp
  ${CMAKE_CURRENT_BINARY_DIR}/steam_share_builtin.cpp
  src/fx/classic.cpp
  src/fx/hoist.cpp
  src/fx/blend.cpp)
target_link_libraries(sopt-fx-lib PUBLIC reshadefx sopt-search)
sopt_fp_flags(sopt-fx-lib)

add_executable(sopt-fx src/cli/fx_main.cpp src/cli/console.cpp)
target_link_libraries(sopt-fx PRIVATE sopt-fx-lib sopt-measure)
target_compile_definitions(sopt-fx PRIVATE SOPT_VERSION="${PROJECT_VERSION}")
sopt_fp_flags(sopt-fx)

add_executable(sopt src/cli/main.cpp src/cli/console.cpp)
target_link_libraries(sopt PRIVATE sopt-search sopt-measure)
target_compile_definitions(sopt PRIVATE SOPT_VERSION="${PROJECT_VERSION}")
sopt_fp_flags(sopt)

if(SOPT_BUILD_BENCH)
  add_executable(sopt-bench bench/bench.cpp)
  target_link_libraries(sopt-bench PRIVATE sopt-search)
  sopt_fp_flags(sopt-bench)
endif()

# sopt-fxc (backend normalization, DXBC after Microsoft's fxc): Windows only; for Linux
# build it with mingw and run it under Wine (tools/fxc/README.md).
if(WIN32)
  enable_language(C)
  add_executable(sopt-fxc tools/fxc/sopt_fxc.c)
  target_link_libraries(sopt-fxc PRIVATE d3dcompiler)

  # M4 benchmark harness (tools/windows/timer/README.md): sopt-timer, a ReShade add-on (needs
  # ReShade with full add-on support), and sopt-host, a Direct3D 9-12 / Vulkan / OpenGL window showing one
  # fixed image with vsync off.
  add_library(sopt-timer SHARED tools/windows/timer/sopt_timer.cpp)
  target_include_directories(sopt-timer PRIVATE third_party/reshade-addon/include third_party/reshade-addon/imgui)
  target_compile_features(sopt-timer PRIVATE cxx_std_17)
  if(CMAKE_SIZEOF_VOID_P EQUAL 8)
    set_target_properties(sopt-timer PROPERTIES PREFIX "" SUFFIX ".addon64")
  else()
    set_target_properties(sopt-timer PROPERTIES PREFIX "" SUFFIX ".addon32")
  endif()
  add_executable(sopt-host tools/windows/host/sopt_host.cpp)
  target_include_directories(sopt-host PRIVATE third_party/vulkan)
  target_compile_features(sopt-host PRIVATE cxx_std_17)
  target_link_libraries(sopt-host PRIVATE d3d9 d3d10 d3d11 dxgi ole32 windowscodecs opengl32)
  # OpBench (sopt-opbench until 0.2.0): instruction costs on the local GPU (D3D11 compute,
  # tools/windows/README.md); target opbench, file OpBench.exe.
  add_executable(opbench tools/windows/opbench/sopt_opbench.cpp)
  set_target_properties(opbench PROPERTIES OUTPUT_NAME OpBench)
  target_compile_features(opbench PRIVATE cxx_std_17)
  target_link_libraries(opbench PRIVATE d3d11 dxgi d3dcompiler)
  target_compile_definitions(opbench PRIVATE SOPT_VERSION="${PROJECT_VERSION}")
  # TexBench (owner, 2026-10-04): texture reads, derivatives and render target writes per format,
  # tools/windows/texbench; shares tools/windows/benchkit.hpp with OpBench.
  add_executable(texbench tools/windows/texbench/texbench.cpp)
  set_target_properties(texbench PROPERTIES OUTPUT_NAME TexBench)
  target_compile_features(texbench PRIVATE cxx_std_17)
  target_link_libraries(texbench PRIVATE d3d11 dxgi d3dcompiler ole32 windowscodecs)
  target_compile_definitions(texbench PRIVATE SOPT_VERSION="${PROJECT_VERSION}")
  # ShaderInfo: what the Vulkan drivers report about compiled shaders (statistics, disassembly, hardware
  # counters); tools/windows/shaderinfo. Vulkan loaded at run time.
  add_executable(shaderinfo tools/windows/shaderinfo/shaderinfo.cpp)
  set_target_properties(shaderinfo PROPERTIES OUTPUT_NAME ShaderInfo)
  target_include_directories(shaderinfo PRIVATE third_party/vulkan)
  target_compile_features(shaderinfo PRIVATE cxx_std_17)
  target_link_libraries(shaderinfo PRIVATE d3d11 dxgi)
  target_compile_definitions(shaderinfo PRIVATE SOPT_VERSION="${PROJECT_VERSION}")
  # Version resource (name, publisher, version under Properties > Details) on every tool we
  # distribute, and a manifest (asInvoker, supported Windows versions) on the executables:
  # unsigned executables without them look more suspicious to Windows Defender.
  function(sopt_version_info target filename description filetype product)
    set(SOPT_RC_PRODUCT ${product})
    set(SOPT_RC_NAME ${target})
    set(SOPT_RC_FILENAME ${filename})
    set(SOPT_RC_DESCRIPTION ${description})
    set(SOPT_RC_FILETYPE ${filetype})
    configure_file(tools/windows/version.rc.in ${CMAKE_CURRENT_BINARY_DIR}/${target}_version.rc @ONLY)
    target_sources(${target} PRIVATE ${CMAKE_CURRENT_BINARY_DIR}/${target}_version.rc)
    if(filetype STREQUAL "VFT_APP")
      target_sources(${target} PRIVATE tools/windows/app.manifest)
    endif()
  endfunction()
  sopt_version_info(opbench OpBench.exe "OpBench: GPU instruction cost benchmark (GPU Blueprint)" VFT_APP "GPU Blueprint")
  sopt_version_info(texbench TexBench.exe "TexBench: GPU texture cost benchmark (GPU Blueprint)" VFT_APP "GPU Blueprint")
  sopt_version_info(shaderinfo ShaderInfo.exe "ShaderInfo: what GPU drivers report about compiled shaders (GPU Blueprint)" VFT_APP "GPU Blueprint")
  sopt_version_info(sopt-host sopt-host.exe "SweetOpt test host (Direct3D 9-12, Vulkan, OpenGL)" VFT_APP "SweetOpt")
  sopt_version_info(sopt-fxc sopt-fxc.exe "SweetOpt helper: HLSL to DXBC with Microsoft fxc" VFT_APP "SweetOpt")
  sopt_version_info(sopt sopt.exe "SweetOpt (sopt): super sweet shader optimizer, single expressions" VFT_APP "SweetOpt")
  sopt_version_info(sopt-fx sopt-fx.exe "SweetOpt (sopt-fx): super sweet shader optimizer for ReShade FX and HLSL" VFT_APP "SweetOpt")
  if(CMAKE_SIZEOF_VOID_P EQUAL 8)
    sopt_version_info(sopt-timer sopt-timer.addon64 "SweetOpt GPU timer add-on for ReShade" VFT_DLL "SweetOpt")
  else()
    sopt_version_info(sopt-timer sopt-timer.addon32 "SweetOpt GPU timer add-on for ReShade" VFT_DLL "SweetOpt")
  endif()
  # UTF-8 string literals (OpBench's block graphics): without /utf-8 MSVC converts them
  # to the ANSI code page at compile time and writes '?' (0.1.0); C4566 as an error catches that.
  if(MSVC)
    foreach(t opbench texbench shaderinfo sopt-host sopt-fxc sopt-timer)
      target_compile_options(${t} PRIVATE /utf-8 /we4566)
    endforeach()
  endif()
  # Dynamic C runtime (the default): Windows Defender flagged the static-CRT build as
  # Trojan:Win32/Sabsik.FL.A!ml (ML false positive) and not this one (owner, 2026-10-02).
  # Needs the VC++ 2015-2022 redistributable, which nearly every gaming PC has.
endif()

if(SOPT_BUILD_TESTS)
  enable_testing()
  add_executable(sopt-tests
    tests/test_main.cpp
    tests/test_eval.cpp
    tests/test_parser.cpp
    tests/test_bits.cpp
    tests/test_rewrites.cpp
    tests/test_isa.cpp
    tests/test_sass.cpp
    tests/test_backends.cpp
    tests/test_vector.cpp
    tests/test_accuracy.cpp
    tests/test_bound.cpp
    tests/test_library.cpp
    tests/test_schedule.cpp
    tests/test_fx.cpp)
  target_link_libraries(sopt-tests PRIVATE sopt-search sopt-measure sopt-fx-lib)
  target_compile_definitions(sopt-tests PRIVATE SOPT_EXAMPLES_DIR="${CMAKE_CURRENT_SOURCE_DIR}/examples"
    SOPT_TESTS_DIR="${CMAKE_CURRENT_SOURCE_DIR}/tests"
    SOPT_LIBRARY_FILE="${CMAKE_CURRENT_SOURCE_DIR}/library/rewrites.txt")
  sopt_fp_flags(sopt-tests)
  add_test(NAME sopt-tests COMMAND sopt-tests)
endif()

