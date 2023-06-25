set(TARGET_NAME "usecase2")

file(
    GLOB_RECURSE SOURCES
    *.h *.hpp *.c *.cpp)

file(
    GLOB FPNG_SOURCES
    "${CMAKE_SOURCE_DIR}/ext/fpng/src/fpng.h"
    "${CMAKE_SOURCE_DIR}/ext/fpng/src/fpng.cpp"
)

source_group("ext/fpng" FILES ${FPNG_SOURCES})

add_executable(
    "${TARGET_NAME}"
    ${SOURCES}
    ${FPNG_SOURCES}
)
target_compile_features("${TARGET_NAME}" PRIVATE cxx_std_20)
set_target_properties("${TARGET_NAME}" PROPERTIES CXX_EXTENSIONS OFF)
target_include_directories(
    "${TARGET_NAME}" PRIVATE
    "../../ext/fpng/src"
    "../../ext/stb"
)
