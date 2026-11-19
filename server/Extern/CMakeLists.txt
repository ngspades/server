if (GIT_SUBMODULES_FETCH)
  find_package(Git REQUIRED)

  if(NOT EXISTS ${PROJECT_SOURCE_DIR}/../common/vxl/CMakeLists.txt OR NOT EXISTS ${PROJECT_SOURCE_DIR}/../common/enet/CMakeLists.txt)
    execute_process(COMMAND ${GIT_EXECUTABLE} submodule update --init --recursive -- ${dir}
      WORKING_DIRECTORY ${PROJECT_SOURCE_DIR}
      COMMAND_ERROR_IS_FATAL ANY)
  endif()
endif()

# the map is libvxl, shared with the client (common/vxl, docs/map-memory.md);
# libmapvxl stays only as tools/mapbench's baseline. libvxl asks for CMake
# 2.8 and builds with -Ofast: quiet for it alone, as the client does
set(_policy_minimum ${CMAKE_POLICY_VERSION_MINIMUM})
set(CMAKE_POLICY_VERSION_MINIMUM 3.10)
add_subdirectory(${PROJECT_SOURCE_DIR}/../common/vxl ${CMAKE_BINARY_DIR}/vxl)
set(CMAKE_POLICY_VERSION_MINIMUM ${_policy_minimum})
include(CheckCCompilerFlag)
check_c_compiler_flag(-Wno-deprecated-ofast HAVE_WNO_DEPRECATED_OFAST)
if(HAVE_WNO_DEPRECATED_OFAST)
    target_compile_options(vxl PRIVATE -Wno-deprecated-ofast)
endif()
# the ENet fork is shared with the client (common/enet)
add_subdirectory(${PROJECT_SOURCE_DIR}/../common/enet ${CMAKE_BINARY_DIR}/enet)
add_library(enet ALIAS enet6)
add_subdirectory(tomlc99)

# third-party code kept as it came (docs: Extern/THIRD_PARTY.md); uthash is
# headers only and is found through the Extern include path
add_library(mersenne_twister STATIC mersenne-twister/MT.c)
target_include_directories(mersenne_twister PUBLIC ${CMAKE_CURRENT_SOURCE_DIR})
