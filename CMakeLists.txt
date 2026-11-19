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

add_library(browsermem SHARED
  native/src/browsers.cpp
  native/src/c_api.cpp
  native/src/common.cpp
  native/src/memory.cpp
  native/src/monitor.cpp
  native/src/resolver.cpp
  native/src/url_index.cpp)
target_include_directories(browsermem
  PUBLIC  $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/native/include> $<INSTALL_INTERFACE:include>
  PRIVATE native/src)
target_compile_definitions(browsermem PRIVATE BM_BUILD_DLL BM_VERSION="${BM_VERSION}"
                                              UNICODE _UNICODE)
target_compile_options(browsermem PRIVATE /W4 /WX /utf-8 /permissive- /EHsc)
target_link_libraries(browsermem PRIVATE user32)

if(SKBUILD)
  # Python wheel: the DLL sits next to the package's Python files.
  install(TARGETS browsermem RUNTIME DESTINATION browsermem)
else()
  # Plain C/C++ consumers: cmake --install build --prefix <dir>
  install(TARGETS browsermem RUNTIME DESTINATION bin ARCHIVE DESTINATION lib)
  install(FILES native/include/browsermem.h DESTINATION include)
endif()
