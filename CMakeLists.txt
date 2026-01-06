
cmake_minimum_required(VERSION 3.13)
project(leaudiod C)

find_package(PkgConfig REQUIRED)
pkg_check_modules(GLIB REQUIRED glib-2.0 gio-2.0)
pkg_check_modules(ALSA REQUIRED alsa)

# liblc3 is installed by us; allow detection if pkg-config is missing
find_library(LC3_LIB lc3 REQUIRED)

add_executable(leaudiod
    src/leaudiod.c
    src/lc3_pipe.c
)
target_include_directories(leaudiod PRIVATE ${GLIB_INCLUDE_DIRS} ${ALSA_INCLUDE_DIRS} include)
target_link_libraries(leaudiod PRIVATE ${GLIB_LIBRARIES} ${ALSA_LIBRARIES} ${LC3_LIB})
add_compile_options(${GLIB_CFLAGS_OTHER} ${ALSA_CFLAGS_OTHER})
