cmake_minimum_required(VERSION 3.20)
project(sopt LANGUAGES CXX)

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
  src/verify/verify.cpp)
target_link_libraries(sopt-verify PUBLIC sopt-ir Threads::Threads)
sopt_fp_flags(sopt-verify)

add_library(sopt-search
  src/search/enumerator.cpp
  src/search/driver.cpp)
target_link_libraries(sopt-search PUBLIC sopt-verify)
sopt_fp_flags(sopt-search)

add_library(sopt-measure
  src/measure/tools.cpp
  src/measure/isa.cpp
  src/measure/sass.cpp)
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
  src/fx/variants.cpp)
target_link_libraries(sopt-fx-lib PUBLIC reshadefx sopt-search)
sopt_fp_flags(sopt-fx-lib)

add_executable(sopt-fx src/cli/fx_main.cpp)
target_link_libraries(sopt-fx PRIVATE sopt-fx-lib sopt-measure)
sopt_fp_flags(sopt-fx)

add_executable(sopt src/cli/main.cpp)
target_link_libraries(sopt PRIVATE sopt-search sopt-measure)
sopt_fp_flags(sopt)

if(SOPT_BUILD_BENCH)
  add_executable(sopt-bench bench/bench.cpp)
  target_link_libraries(sopt-bench PRIVATE sopt-search)
  sopt_fp_flags(sopt-bench)
endif()

if(SOPT_BUILD_TESTS)
  enable_testing()
  add_executable(sopt-tests
    tests/test_main.cpp
    tests/test_eval.cpp
    tests/test_parser.cpp
    tests/test_rewrites.cpp
    tests/test_isa.cpp
    tests/test_sass.cpp
    tests/test_vector.cpp
    tests/test_fx.cpp)
  target_link_libraries(sopt-tests PRIVATE sopt-search sopt-measure sopt-fx-lib)
  target_compile_definitions(sopt-tests PRIVATE SOPT_EXAMPLES_DIR="${CMAKE_CURRENT_SOURCE_DIR}/examples"
    SOPT_TESTS_DIR="${CMAKE_CURRENT_SOURCE_DIR}/tests")
  sopt_fp_flags(sopt-tests)
  add_test(NAME sopt-tests COMMAND sopt-tests)
endif()

