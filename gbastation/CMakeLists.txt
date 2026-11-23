cmake_minimum_required(VERSION 3.21)
project(WoPlay_mGBA LANGUAGES C CXX)

set(CMAKE_CXX_STANDARD 17)
set(ROOT "${CMAKE_CURRENT_SOURCE_DIR}/..")
set(FRONTEND "${ROOT}/../WoPlay" CACHE PATH "WoPlay frontend source")
set(TICO "${ROOT}/tico")
set(IMGUI "${FRONTEND}/third_party/imgui")
set(GBS_CORE_SDK_ONLY ON CACHE BOOL "" FORCE)
set(BUILD_SHARED_LIBS OFF CACHE BOOL "" FORCE)
find_package(Threads REQUIRED)
add_subdirectory("${FRONTEND}" sdk EXCLUDE_FROM_ALL)
set(GBS_THIRD_PARTY "${FRONTEND}/third_party")
set(GBS_VULKAN_GLASS ON)
include("${FRONTEND}/cmake/dependencies/GlassShaders.cmake")

include(FetchContent)
function(woplay_dependency variable name local marker repository revision)
    set(source "${CMAKE_CURRENT_SOURCE_DIR}/deps/${local}")
    if(NOT EXISTS "${source}/${marker}")
        FetchContent_Declare(${name} GIT_REPOSITORY "${repository}" GIT_TAG "${revision}"
            GIT_SUBMODULES "" SOURCE_SUBDIR woplay-source-only)
        FetchContent_MakeAvailable(${name})
        set(source "${${name}_SOURCE_DIR}")
    endif()
    set(${variable} "${source}" PARENT_SCOPE)
endfunction()
woplay_dependency(LRC woplay_libretro libretro-common include/libretro.h
    https://github.com/libretro/libretro-common.git 52781c4a8b5aae19d858f4a2326c8f8308c70f54)

foreach(option IN ITEMS ENABLE_OPT ENABLE_HLSL ENABLE_GLSLANG_BINARIES GLSLANG_TESTS
                        BUILD_EXTERNAL ENABLE_SPVREMAPPER GLSLANG_ENABLE_INSTALL)
    set(${option} OFF CACHE BOOL "" FORCE)
endforeach()
add_subdirectory("${TICO}/deps/glslang" glslang EXCLUDE_FROM_ALL)
set(LIBMGBA_ONLY ON CACHE BOOL "" FORCE)
set(BUILD_LIBRETRO ON CACHE BOOL "" FORCE)
set(LIBRETRO_STATIC ON CACHE BOOL "" FORCE)
add_subdirectory("${ROOT}" mgba EXCLUDE_FROM_ALL)

file(GLOB rcheevos_sources "${ROOT}/rcheevos/src/*.c" "${ROOT}/rcheevos/src/*/*.c")
list(FILTER rcheevos_sources EXCLUDE REGEX "rc_client_external\\.c$")
add_library(rcheevos STATIC ${rcheevos_sources})
target_include_directories(rcheevos PUBLIC "${ROOT}/rcheevos/include" PRIVATE "${LRC}/include")
target_compile_definitions(rcheevos PUBLIC RC_CLIENT_SUPPORTS_HASH)

set(sources
    session_host.cpp menu_host.cpp state_store.cpp menu_effects.cpp
    "${TICO}/TicoMain.cpp" "${TICO}/TicoCore.cpp" "${TICO}/UsbStorage.cpp"
    "${TICO}/TicoVulkan.cpp" "${TICO}/TicoShaderChain.cpp" "${TICO}/TicoSlang.cpp"
    "${TICO}/overlay/imgui_overlay.cpp" "${TICO}/overlay/overlay_ui.cpp" "${TICO}/overlay/ra_alerts.cpp"
    "${TICO}/overlay/tico_config.cpp" "${TICO}/overlay/translation_manager.cpp"
    "${TICO}/deps/SPIRV-Reflect/spirv_reflect.c" "${IMGUI}/backends/imgui_impl_vulkan.cpp")
if(GBS_PLATFORM STREQUAL "switch")
    list(APPEND sources "${TICO}/TicoStubs.cpp")
else()
    list(APPEND sources "${TICO}/deps/volk/volk.c" "${IMGUI}/backends/imgui_impl_sdl2.cpp")
endif()
add_executable(woplay-mgba ${sources})
target_include_directories(woplay-mgba PRIVATE "${CMAKE_CURRENT_BINARY_DIR}/generated"
    "${FRONTEND}/modules/rendering/src/effects" "${IMGUI}" "${IMGUI}/backends"
    "${FRONTEND}/third_party/json/include/nlohmann"
    "${TICO}" "${TICO}/deps" "${TICO}/deps/volk" "${TICO}/deps/vulkan-headers"
    "${TICO}/deps/SPIRV-Reflect" "${ROOT}/src/platform/libretro"
    "${LRC}/include")
target_compile_definitions(woplay-mgba PRIVATE WOPLAY_HOST IMGUI_IMPL_VULKAN_NO_PROTOTYPES
    TICO_APP_VERSION="3.0.2-woplay.1")
target_link_libraries(woplay-mgba PRIVATE gbs::core_ui mgba_libretro rcheevos glslang
    glslang-default-resource-limits Threads::Threads)

if(GBS_PLATFORM STREQUAL "switch")
    set(MESA_NVK_DIR "${ROOT}/../mesa-switch/builddir-gbs-switch" CACHE PATH "Switch Mesa NVK build")
    set(NVK "${MESA_NVK_DIR}/src/nouveau/vulkan/libvulkan.a")
    if(NOT EXISTS "${NVK}")
        message(FATAL_ERROR "Set MESA_NVK_DIR to the Switch Mesa NVK build")
    endif()
    target_compile_definitions(woplay-mgba PRIVATE VK_USE_PLATFORM_VI_NN DISABLE_LOGGING LIBARCHIVE_STATIC)
    target_include_directories(woplay-mgba PRIVATE "$ENV{DEVKITPRO}/portlibs/switch/include" "$ENV{DEVKITPRO}/portlibs/switch/include/SDL2"
        "${TICO}/deps/usbhsfs/include")
    target_link_directories(woplay-mgba PRIVATE "$ENV{DEVKITPRO}/portlibs/switch/lib" "${TICO}/deps/usbhsfs/lib"
        "${MESA_NVK_DIR}/src/nouveau/lib")
    target_link_libraries(woplay-mgba PRIVATE "${NVK}" SDL2_mixer mpg123 modplug opusfile opus vorbisidec ogg
        SDL2 archive curl sodium mbedtls mbedx509 mbedcrypto expat bz2 lzma lz4 z zstd usbhsfs usbntfs nx m)
    set_target_properties(woplay-mgba PROPERTIES SUFFIX ".elf")
    find_program(ELF2NRO elf2nro HINTS "$ENV{DEVKITPRO}/tools/bin" REQUIRED)
    find_program(NACPTOOL nacptool HINTS "$ENV{DEVKITPRO}/tools/bin" REQUIRED)
    set(TICO_ROMFS "${CMAKE_CURRENT_BINARY_DIR}/tico-romfs")
    set(SIDEBAR_FONT "${FRONTEND}/assets/fonts/MaterialIcons-Regular.ttf")
    file(GLOB_RECURSE tico_romfs_assets CONFIGURE_DEPENDS
        "${TICO}/assets/*" "${TICO}/fonts/*" "${TICO}/lang/*" "${TICO}/shaders/*")
    add_custom_command(OUTPUT core.nacp
        COMMAND "${NACPTOOL}" --create "WoPlay mGBA" "WoPlay" "3.0.2" core.nacp VERBATIM)
    add_custom_command(OUTPUT woplay-mgba.nro
        COMMAND "${CMAKE_COMMAND}" -E make_directory "${TICO_ROMFS}/module"
        COMMAND "${CMAKE_COMMAND}" -E copy_directory "${TICO}/assets" "${TICO_ROMFS}/assets"
        COMMAND "${CMAKE_COMMAND}" -E copy_directory "${TICO}/fonts" "${TICO_ROMFS}/fonts"
        COMMAND "${CMAKE_COMMAND}" -E copy_directory "${TICO}/lang" "${TICO_ROMFS}/lang"
        COMMAND "${CMAKE_COMMAND}" -E copy_directory "${TICO}/shaders" "${TICO_ROMFS}/shaders"
        COMMAND "${CMAKE_COMMAND}" -E copy "${TICO}/module/settings.json" "${TICO_ROMFS}/module/settings.json"
        COMMAND "${CMAKE_COMMAND}" -E copy "${SIDEBAR_FONT}" "${TICO_ROMFS}/fonts/"
        COMMAND "${ELF2NRO}" "$<TARGET_FILE:woplay-mgba>" woplay-mgba.nro --nacp=core.nacp --romfsdir=${TICO_ROMFS}
        DEPENDS woplay-mgba core.nacp ${tico_romfs_assets} "${SIDEBAR_FONT}" "${TICO}/module/settings.json" VERBATIM)
    add_custom_target(woplay_mgba_nro ALL DEPENDS woplay-mgba.nro)
    install(FILES "${CMAKE_CURRENT_BINARY_DIR}/woplay-mgba.nro" DESTINATION .)
else()
    find_package(SDL2 REQUIRED CONFIG)
    find_package(CURL REQUIRED)
    find_library(MOLTENVK MoltenVK REQUIRED)
    woplay_dependency(MIXER_SOURCE woplay_mixer SDL_mixer CMakeLists.txt
        https://github.com/libsdl-org/SDL_mixer.git 10e46b2ffe798159e6c5d386bfc63364cd155b93)
    woplay_dependency(ARCHIVE_SOURCE woplay_archive libarchive CMakeLists.txt
        https://github.com/libarchive/libarchive.git 4a5944b726d24c8e86589ad09cef958c6f1a6e9c)
    woplay_dependency(SODIUM_SOURCE woplay_sodium libsodium configure
        https://github.com/jedisct1/libsodium.git db1238f0a509de16b4bc323def5e68fdee78adda)
    foreach(feature IN ITEMS FLAC GME MOD MP3 MIDI OPUS WAVPACK VORBIS CMD SAMPLES INSTALL)
        set(SDL2MIXER_${feature} OFF CACHE BOOL "" FORCE)
    endforeach()
    add_subdirectory("${MIXER_SOURCE}" mixer EXCLUDE_FROM_ALL)
    foreach(feature IN ITEMS TEST INSTALL BZip2 LIBXML2 EXPAT OPENSSL NETTLE CNG LIBB2 LZ4 ZSTD TAR CPIO CAT UNZIP)
        set(ENABLE_${feature} OFF CACHE BOOL "" FORCE)
    endforeach()
    add_subdirectory("${ARCHIVE_SOURCE}" archive EXCLUDE_FROM_ALL)
    include(ExternalProject)
    set(SODIUM_PREFIX "${CMAKE_CURRENT_BINARY_DIR}/sodium")
    file(MAKE_DIRECTORY "${SODIUM_PREFIX}/include")
    ExternalProject_Add(sodium_build SOURCE_DIR "${SODIUM_SOURCE}"
        CONFIGURE_COMMAND <SOURCE_DIR>/configure --prefix=${SODIUM_PREFIX} --disable-shared --enable-static
        BUILD_COMMAND make -j4 INSTALL_COMMAND make install
        BUILD_BYPRODUCTS "${SODIUM_PREFIX}/lib/libsodium.a")
    add_library(sodium_local STATIC IMPORTED)
    set_target_properties(sodium_local PROPERTIES IMPORTED_LOCATION "${SODIUM_PREFIX}/lib/libsodium.a"
        INTERFACE_INCLUDE_DIRECTORIES "${SODIUM_PREFIX}/include")
    add_dependencies(sodium_local sodium_build)
    target_compile_definitions(woplay-mgba PRIVATE VK_NO_PROTOTYPES WOPLAY_MOLTENVK="${MOLTENVK}")
    target_link_libraries(woplay-mgba PRIVATE SDL2::SDL2 SDL2_mixer::SDL2_mixer-static
        CURL::libcurl archive_static sodium_local ${CMAKE_DL_LIBS})
    install(TARGETS woplay-mgba RUNTIME DESTINATION .)
endif()

install(FILES module/module.json "${TICO}/module/settings.json" DESTINATION .)
install(DIRECTORY "${TICO}/lang" "${TICO}/shaders" "${TICO}/module/gamelists" DESTINATION .)
install(DIRECTORY "${FRONTEND}/assets/fonts" DESTINATION ui/assets)
install(FILES "${FRONTEND}/assets/frontend/focus_ring_gradient.png"
    "${FRONTEND}/assets/frontend/avatar.png" DESTINATION ui/assets/frontend)
