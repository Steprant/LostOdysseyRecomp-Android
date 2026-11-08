cmake_minimum_required(VERSION 3.28)
project(LoSgsr2Readbacks LANGUAGES C CXX)
if(NOT CMAKE_SYSTEM_NAME STREQUAL "Linux")
    message(FATAL_ERROR "This standalone headless fixture currently targets Linux Vulkan")
endif()
if(NOT CMAKE_BUILD_TYPE)
    set(CMAKE_BUILD_TYPE Release CACHE STRING "Build type" FORCE)
endif()
get_filename_component(ROOT "${CMAKE_CURRENT_LIST_DIR}/../../.." ABSOLUTE)
set(LO_SGSR2_DEPENDENCY_ROOT "${ROOT}" CACHE PATH "Checkout with prepared pinned dependencies")
set(DEPS "${LO_SGSR2_DEPENDENCY_ROOT}")
set(CMAKE_CXX_STANDARD 20)
find_package(Threads REQUIRED)
find_package(X11 REQUIRED)
set(PLUME_SDL_VULKAN_ENABLED OFF CACHE BOOL "" FORCE)
add_subdirectory("${DEPS}/thirdparty/plume" plume)
# Production controller, shader source and DXC path; only the runtime PCH is
# replaced (shared with the presentation fixture).
add_executable(LoSgsr2Test
    ../sgsr2_test.cpp ../motion_replay/platform_stub.cpp
    "${ROOT}/LostOdysseyRecomp/gpu/sgsr2_upscaler.cpp"
    "${ROOT}/LostOdysseyRecomp/gpu/shader/dxc_compiler.cpp")
target_include_directories(LoSgsr2Test BEFORE PRIVATE ../presentation/stubs
    "${ROOT}/LostOdysseyRecomp"
    "${DEPS}/tools/XenonRecomp/thirdparty/fmt/include"
    "${DEPS}/tools/XenonRecomp/thirdparty/xxHash"
    "${DEPS}/tools/XenosRecomp/thirdparty/dxc-bin/inc")
target_compile_definitions(LoSgsr2Test PRIVATE LO_GPU_PLUME NOMINMAX FMT_HEADER_ONLY XXH_INLINE_ALL)
target_link_libraries(LoSgsr2Test PRIVATE plume X11::X11 Threads::Threads ${CMAKE_DL_LIBS})
enable_testing()
add_test(NAME sgsr2_controller COMMAND LoSgsr2Test)
set_tests_properties(sgsr2_controller PROPERTIES TIMEOUT 180 ENVIRONMENT
    "LO_SHADER_CACHE_DIR=${CMAKE_CURRENT_BINARY_DIR}/shader-cache"
    ENVIRONMENT_MODIFICATION "LD_LIBRARY_PATH=path_list_prepend:${DEPS}/tools/XenosRecomp/thirdparty/dxc-bin/lib/x64")
# Plume's kill-free fragment modules (pipelines of draws without the alpha test).
add_executable(LoKillFreeModuleTest ../kill_free_module_test.cpp ../motion_replay/platform_stub.cpp
    "${ROOT}/LostOdysseyRecomp/gpu/shader/dxc_compiler.cpp")
target_include_directories(LoKillFreeModuleTest BEFORE PRIVATE ../presentation/stubs
    "${ROOT}/LostOdysseyRecomp"
    "${DEPS}/tools/XenonRecomp/thirdparty/fmt/include"
    "${DEPS}/tools/XenonRecomp/thirdparty/xxHash"
    "${DEPS}/tools/XenosRecomp/thirdparty/dxc-bin/inc")
target_compile_definitions(LoKillFreeModuleTest PRIVATE LO_GPU_PLUME NOMINMAX FMT_HEADER_ONLY XXH_INLINE_ALL)
target_link_libraries(LoKillFreeModuleTest PRIVATE plume X11::X11 Threads::Threads ${CMAKE_DL_LIBS})
add_test(NAME kill_free_modules COMMAND LoKillFreeModuleTest)
set_tests_properties(kill_free_modules PROPERTIES TIMEOUT 120 ENVIRONMENT
    "LO_SHADER_CACHE_DIR=${CMAKE_CURRENT_BINARY_DIR}/shader-cache"
    ENVIRONMENT_MODIFICATION "LD_LIBRARY_PATH=path_list_prepend:${DEPS}/tools/XenosRecomp/thirdparty/dxc-bin/lib/x64")
# Roadmap 1.1: Plume's attachment content census (RenderPassProfile::ContentFlow).
add_executable(LoPassContentCensusTest ../pass_content_census_test.cpp)
target_link_libraries(LoPassContentCensusTest PRIVATE plume X11::X11 Threads::Threads ${CMAKE_DL_LIBS})
add_test(NAME pass_content_census COMMAND LoPassContentCensusTest)
set_tests_properties(pass_content_census PROPERTIES TIMEOUT 60)
add_test(NAME pass_content_census_load_kept COMMAND LoPassContentCensusTest)
set_tests_properties(pass_content_census_load_kept PROPERTIES TIMEOUT 60 ENVIRONMENT "LO_VK_DISCARD_UNDEFINED=0")
add_test(NAME pass_content_census_full_area COMMAND LoPassContentCensusTest)
set_tests_properties(pass_content_census_full_area PROPERTIES TIMEOUT 60 ENVIRONMENT "LO_VK_RENDER_AREA=0")
