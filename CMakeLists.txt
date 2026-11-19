cmake_minimum_required(VERSION 3.20)

# The version lives in pyproject.toml; scikit-build-core passes it in.
if(DEFINED SKBUILD_PROJECT_VERSION)
  set(BM_VERSION "${SKBUILD_PROJECT_VERSION}")
else()
  set(BM_VERSION "0.0.0")
endif()

project(browsermem VERSION ${BM_VERSION} LANGUAGES CXX)

if(NOT WIN32)
  message(FATAL_ERROR "browsermem currently supports Windows only")
endif()

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)
# Link the C runtime statically so the DLL has no redistributable dependency.
set(CMAKE_MSVC_RUNTIME_LIBRARY "MultiThreaded$<$<CONFIG:Debug>:Debug>")

option(BROWSERMEM_BUILD_TOOLS "Build developer tools (benchmark)" OFF)

# Engine shared by the DLL and the developer tools.
add_library(browsermem_core OBJECT
  native/src/browsers.cpp
  native/src/common.cpp
  native/src/memory.cpp
  native/src/memory_source.cpp
  native/src/monitor.cpp
  native/src/resolver.cpp
  native/src/url_index.cpp
  native/src/url_syntax.cpp)
target_include_directories(browsermem_core PUBLIC native/src native/include)
target_compile_definitions(browsermem_core PUBLIC UNICODE _UNICODE)
target_compile_options(browsermem_core PUBLIC /W4 /WX /utf-8 /permissive- /EHsc)

add_library(browsermem SHARED native/src/c_api.cpp $<TARGET_OBJECTS:browsermem_core>)
target_include_directories(browsermem
  PUBLIC  $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/native/include> $<INSTALL_INTERFACE:include>
  PRIVATE native/src)
target_compile_definitions(browsermem PRIVATE BM_BUILD_DLL BM_VERSION="${BM_VERSION}"
                                              UNICODE _UNICODE)
target_compile_options(browsermem PRIVATE /W4 /WX /utf-8 /permissive- /EHsc)
target_link_libraries(browsermem PRIVATE user32)

if(BROWSERMEM_BUILD_TOOLS)
  add_executable(browsermem_bench native/tools/bench.cpp $<TARGET_OBJECTS:browsermem_core>)
  target_link_libraries(browsermem_bench PRIVATE browsermem_core user32)
endif()

if(SKBUILD)
  # Python wheel: the DLL sits next to the package's Python files.
  install(TARGETS browsermem RUNTIME DESTINATION browsermem)
else()
  # Plain C/C++ consumers: cmake --install build --prefix <dir>
  install(TARGETS browsermem RUNTIME DESTINATION bin ARCHIVE DESTINATION lib)
  install(FILES native/include/browsermem.h DESTINATION include)
endif()
