cmake_minimum_required(VERSION 3.7)
project(Filters)

include(filters.cmake)

add_definitions(-fpic)

SET(LIBDAVS2_SRC
        ${CMAKE_CURRENT_SOURCE_DIR}/dec_avs2.c
        ${CMAKE_CURRENT_SOURCE_DIR}/avs2_stubs.c
)

SET(LIBDAVS2_LIBS
        ${CMAKE_CURRENT_SOURCE_DIR}/lib/libdavs2.a
)

SET(LIBDAVS2_INC
        ${CMAKE_CURRENT_SOURCE_DIR}/include
)

# davs2's headers include <xmmintrin.h> even with --disable-asm, so the library
# is built against emscripten's SSE emulation and the filter has to be linked
# with the matching SIMD support.
add_filter(libdavs2
        "${LIBDAVS2_SRC}"
        "${LIBDAVS2_LIBS}"
        []
        ""
        "${LIBDAVS2_INC}"
        "-msimd128"
        "1")
