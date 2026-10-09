cmake_minimum_required(VERSION 3.16)
project(descompactador_gzip VERSION 1.0.0 LANGUAGES C)

set(CMAKE_C_STANDARD 11)
set(CMAKE_C_STANDARD_REQUIRED ON)
set(CMAKE_C_EXTENSIONS OFF)
if(CMAKE_C_COMPILER_ID MATCHES "Clang|GNU")
  add_compile_options(-Wall -Wextra -Wpedantic)
endif()

add_library(mgz STATIC
  src/bitio.c src/crc32.c src/deflate.c src/gzip.c
  src/huffman.c src/inflate.c src/tables.c)
target_include_directories(mgz PUBLIC include PRIVATE src)

add_executable(mini-gzip src/main.c)
target_link_libraries(mini-gzip PRIVATE mgz)

enable_testing()

add_executable(test_unidade tests/test_unidade.c)
target_include_directories(test_unidade PRIVATE src)
target_link_libraries(test_unidade PRIVATE mgz)

add_executable(gerador tests/gerador.c)

add_test(NAME unidade COMMAND test_unidade)
add_test(NAME diferencial
  COMMAND sh ${CMAKE_CURRENT_SOURCE_DIR}/tests/diferencial.sh
          $<TARGET_FILE:mini-gzip> $<TARGET_FILE:gerador>)
