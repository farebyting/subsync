cmake_minimum_required(VERSION 3.20)
project(subsync CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

if(MSVC)
  add_compile_options(/W4 /permissive-)
else()
  add_compile_options(-Wall -Wextra)
  # Produce a genuinely self-contained binary: the MinGW toolchain's libc++,
  # libunwind and winpthread normally live in DLLs that are not on PATH for a
  # user who only has the built executable.
  add_link_options(-static-libgcc -static-libstdc++)
  if(MINGW)
    add_link_options(-static)
  endif()
endif()

file(GLOB SUB_SRC CONFIGURE_DEPENDS "src/*.cpp")
list(REMOVE_ITEM SUB_SRC "${CMAKE_CURRENT_SOURCE_DIR}/src/main.cpp")

add_library(subsync_core STATIC ${SUB_SRC})
target_include_directories(subsync_core PUBLIC src)

add_executable(subsync src/main.cpp)
target_link_libraries(subsync PRIVATE subsync_core)

enable_testing()
add_subdirectory(tests)
