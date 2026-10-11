file(GLOB TEST_SRC CONFIGURE_DEPENDS "test_*.cpp")

foreach(src ${TEST_SRC})
  get_filename_component(name ${src} NAME_WE)
  add_executable(${name} ${src})
  target_link_libraries(${name} PRIVATE subsync_core)
  target_include_directories(${name} PRIVATE ${CMAKE_CURRENT_SOURCE_DIR})
  target_compile_definitions(${name} PRIVATE
    SUBSYNC_EXE="$<TARGET_FILE:subsync>"
    SUBSYNC_FX="${CMAKE_CURRENT_SOURCE_DIR}/fx")
  # test_e2e drives the real binary, so it must exist first.
  add_dependencies(${name} subsync)
  add_test(NAME ${name} COMMAND ${name})
endforeach()
