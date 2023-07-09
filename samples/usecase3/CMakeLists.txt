set(TARGET_NAME "usecase3")

file(
    GLOB_RECURSE SOURCES
    *.h *.hpp *.c *.cpp)

file(
    GLOB FPNG_SOURCES
    "${CMAKE_SOURCE_DIR}/ext/fpng/src/fpng.h"
    "${CMAKE_SOURCE_DIR}/ext/fpng/src/fpng.cpp"
)

file(
    GLOB ASIO_SOURCES
    "${CMAKE_SOURCE_DIR}/ext/asio/asio/include/asio.hpp"
)

source_group("ext/fpng" FILES ${FPNG_SOURCES})
source_group("ext/asio" FILES ${ASIO_SOURCES})

add_executable(
    "${TARGET_NAME}"
    ${SOURCES}
    ${FPNG_SOURCES}
    ${ASIO_SOURCES}
)
target_compile_features("${TARGET_NAME}" PRIVATE cxx_std_20)
set_target_properties("${TARGET_NAME}" PROPERTIES CXX_EXTENSIONS OFF)
target_include_directories(
    "${TARGET_NAME}" PRIVATE
    "../../ext/asio/asio/include"
    "../../ext/fpng/src"
    "../../ext/stb"
)
