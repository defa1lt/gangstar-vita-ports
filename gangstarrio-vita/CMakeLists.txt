cmake_minimum_required(VERSION 3.14)

if(NOT DEFINED ENV{VITASDK} AND NOT DEFINED CMAKE_TOOLCHAIN_FILE)
  message(FATAL_ERROR
    "VitaSDK is required. Set VITASDK or pass CMAKE_TOOLCHAIN_FILE.")
endif()

if(NOT DEFINED CMAKE_TOOLCHAIN_FILE)
  set(CMAKE_TOOLCHAIN_FILE
      "$ENV{VITASDK}/share/vita.toolchain.cmake"
      CACHE PATH "VitaSDK toolchain")
endif()

project(gangstarrio_vita C CXX)

include("${VITASDK}/share/vita.cmake" REQUIRED)

# Match the proven 9mm/MC2 Vita ports: even diagnostic builds must not leave
# the GL and I/O compatibility layer at CMake's default -O0.  The translated
# game calls these wrappers tens of thousands of times per scene.
if(CMAKE_BUILD_TYPE STREQUAL "Debug")
  set(RIO_OPTIMIZATION_FLAGS -O1 -g3)
else()
  set(RIO_OPTIMIZATION_FLAGS -O3 -g0 -ffast-math)
endif()

set(VITA_APP_NAME "Gangstar Rio Vita")
set(VITA_TITLEID "GSRV00001")
set(VITA_VERSION "00.85")
set(VITA_VPK_NAME "gangstarrio-vita")
set(VITA_MKSFOEX_FLAGS "-d ATTRIBUTE2=12")

set(MC2_LOADER_DIR "${CMAKE_CURRENT_SOURCE_DIR}/third_party/mc2-loader")
set(VITAGL_DIR "${CMAKE_CURRENT_SOURCE_DIR}/third_party/vitaGL")
set(VITAGL_LIB "${VITAGL_DIR}/libvitaGL.a")

# Rio and 9mm use the same Glitch engine vertex layout.  Build the pinned 9mm
# VitaGL fork instead of whichever generic archive happens to be installed in
# VitaSDK: that fork fixes planar client arrays, attribute-base underflow,
# postponed attribute overflow and failed-shader link crashes.
if(CMAKE_BUILD_TYPE STREQUAL "Debug")
  set(VITAGL_MAKE_FLAGS
      "SOFTFP_ABI=1 TEST=1 LOG_ERRORS=2 HAVE_PROFILING=1 HAVE_SHARK_LOG=1 HAVE_SHADER_CACHE=1 NO_SPLASHSCREEN=1")
else()
  set(VITAGL_MAKE_FLAGS "SOFTFP_ABI=1 HAVE_SHADER_CACHE=1 NO_DEBUG=1 NO_SPLASHSCREEN=1")
endif()
separate_arguments(VITAGL_MAKE_ARGS UNIX_COMMAND "${VITAGL_MAKE_FLAGS}")

set(VITAGL_FLAGS_STAMP "${CMAKE_BINARY_DIR}/vitagl_flags.stamp")
set(vitagl_old_flags "")
if(EXISTS "${VITAGL_FLAGS_STAMP}")
  file(READ "${VITAGL_FLAGS_STAMP}" vitagl_old_flags)
endif()
if(NOT vitagl_old_flags STREQUAL VITAGL_MAKE_FLAGS)
  file(GLOB_RECURSE vitagl_stale_objects "${VITAGL_DIR}/source/*.o")
  file(REMOVE ${vitagl_stale_objects} "${VITAGL_LIB}")
  file(WRITE "${VITAGL_FLAGS_STAMP}" "${VITAGL_MAKE_FLAGS}")
endif()

include(ProcessorCount)
ProcessorCount(VITAGL_NPROC)
if(VITAGL_NPROC EQUAL 0)
  set(VITAGL_NPROC 4)
endif()
add_custom_target(vitaGL_build
  COMMAND make -C "${VITAGL_DIR}" -j${VITAGL_NPROC}
          "CC=${CMAKE_CURRENT_SOURCE_DIR}/scripts/arm-vita-eabi-gcc-vitagl"
          "CXX=${CMAKE_CURRENT_SOURCE_DIR}/scripts/arm-vita-eabi-g++"
          "AR=${CMAKE_CURRENT_SOURCE_DIR}/scripts/arm-vita-eabi-gcc-ar"
          ${VITAGL_MAKE_ARGS}
  BYPRODUCTS "${VITAGL_LIB}"
  COMMENT "Building pinned Glitch-engine VitaGL"
  VERBATIM
)
add_library(rio_vitaGL STATIC IMPORTED)
set_target_properties(rio_vitaGL PROPERTIES
  IMPORTED_LOCATION "${VITAGL_LIB}"
  INTERFACE_INCLUDE_DIRECTORIES "${VITAGL_DIR}/source"
)

add_library(mc2_compat STATIC
  "${MC2_LOADER_DIR}/source/dynlib.c"
  "${MC2_LOADER_DIR}/source/reimpl/bits/_ctype.c"
  "${MC2_LOADER_DIR}/source/reimpl/controls.c"
  "${MC2_LOADER_DIR}/source/reimpl/audio.c"
  "${MC2_LOADER_DIR}/source/reimpl/egl.c"
  "${MC2_LOADER_DIR}/source/reimpl/errno.c"
  "${MC2_LOADER_DIR}/source/reimpl/intro_video.c"
  "${MC2_LOADER_DIR}/source/reimpl/io.c"
  "${MC2_LOADER_DIR}/source/reimpl/log.c"
  "${MC2_LOADER_DIR}/source/reimpl/mem.c"
  "${MC2_LOADER_DIR}/source/reimpl/native_activity.c"
  "${MC2_LOADER_DIR}/source/reimpl/pthr.c"
  "${MC2_LOADER_DIR}/source/reimpl/sys.c"
  "${MC2_LOADER_DIR}/source/reimpl/time64.c"
  "${MC2_LOADER_DIR}/source/reimpl/asset_manager.cpp"
  "${MC2_LOADER_DIR}/source/utils/glutil.c"
  "${MC2_LOADER_DIR}/source/utils/logger.c"
  "${MC2_LOADER_DIR}/source/utils/utils.c"
  "${MC2_LOADER_DIR}/lib/sha1/sha1.c"
)
target_include_directories(mc2_compat PRIVATE
  "${MC2_LOADER_DIR}/lib"
  "${MC2_LOADER_DIR}/source"
  "${VITAGL_DIR}/source"
)
target_compile_definitions(mc2_compat PRIVATE
  _GNU_SOURCE
  __POSIX_VISIBLE=999999
  USE_GLSL_SHADERS
  DATA_PATH="ux0:data/gangstarrio/"
  $<$<CONFIG:Debug>:RIO_MEMORY_PROFILE>
)
if(CMAKE_BUILD_TYPE STREQUAL "Debug")
  target_sources(mc2_compat PRIVATE "${MC2_LOADER_DIR}/source/reimpl/pthr_test.c")
endif()
target_compile_options(mc2_compat PRIVATE
  ${RIO_OPTIMIZATION_FLAGS}
  $<$<COMPILE_LANGUAGE:C>:-mfloat-abi=softfp;-std=gnu11;-Wno-deprecated-declarations>
  $<$<COMPILE_LANGUAGE:CXX>:-mfloat-abi=softfp;-std=gnu++20;-Wno-write-strings;-Wno-psabi>
)

add_library(so_util STATIC
  "${MC2_LOADER_DIR}/lib/so_util/so_util.c"
)
target_include_directories(so_util PRIVATE
  "${MC2_LOADER_DIR}/lib"
  "${MC2_LOADER_DIR}/lib/kubridge"
  "${MC2_LOADER_DIR}/lib/so_util"
  "${CMAKE_CURRENT_SOURCE_DIR}/include"
)
target_compile_options(so_util PRIVATE ${RIO_OPTIMIZATION_FLAGS} -mfloat-abi=softfp)

add_library(falso_jni STATIC
  "${MC2_LOADER_DIR}/lib/falso_jni/FalsoJNI.c"
  "${MC2_LOADER_DIR}/lib/falso_jni/FalsoJNI_ImplBridge.c"
  "${MC2_LOADER_DIR}/lib/falso_jni/FalsoJNI_Logger.c"
  "${MC2_LOADER_DIR}/lib/falso_jni/converter.c"
)
target_include_directories(falso_jni PRIVATE
  "${MC2_LOADER_DIR}/lib/falso_jni"
)
target_compile_options(falso_jni PRIVATE ${RIO_OPTIMIZATION_FLAGS} -mfloat-abi=softfp -w)
if(CMAKE_BUILD_TYPE STREQUAL "Debug")
  target_sources(falso_jni PRIVATE "${MC2_LOADER_DIR}/lib/falso_jni/FalsoJNI_refs_test.c")
endif()

add_executable(${PROJECT_NAME}
  source/main.c
  source/diagnostics.c
  source/dialog.c
  source/elf_diagnostics.c
  source/compat_resolver.c
  source/compat_trace.c
  source/game_file.c
  source/game_profile.c
  source/java.c
  source/rio_missing_imports.c
  source/runtime_patch.c
  source/sha256.c
  source/fios.c
)
add_dependencies(${PROJECT_NAME} vitaGL_build)

target_include_directories(${PROJECT_NAME} PRIVATE
  include
  "${MC2_LOADER_DIR}/lib"
  "${MC2_LOADER_DIR}/lib/kubridge"
  "${MC2_LOADER_DIR}/lib/so_util"
  "${MC2_LOADER_DIR}/source"
)
target_compile_features(${PROJECT_NAME} PRIVATE c_std_11)
target_compile_definitions(${PROJECT_NAME} PRIVATE
  RIO_VERSION=\"${VITA_VERSION}\"
  $<$<CONFIG:Debug>:HAVE_SHARK_LOG>
)
target_compile_options(${PROJECT_NAME} PRIVATE
  ${RIO_OPTIMIZATION_FLAGS}
  -Wall
  -Wextra
  -Werror
  -mfloat-abi=softfp
)
# --wrap intercepts cross-object references, including temporary-pool spills.
# Internal allocator calls are observed through VitaGL's recovery logger.
target_link_options(${PROJECT_NAME} PRIVATE
  -mfloat-abi=softfp
  -Wl,--wrap=vgl_file_log
  -Wl,--wrap=vgl_reserve_data_pool
  -Wl,--wrap=gpu_alloc_mapped_aligned_for_cpu
  -Wl,--wrap=gpu_alloc_mapped_aligned_for_gpu
  $<$<CONFIG:Debug>:-Wl,--wrap=malloc>
  $<$<CONFIG:Debug>:-Wl,--wrap=calloc>
  $<$<CONFIG:Debug>:-Wl,--wrap=realloc>
  $<$<CONFIG:Debug>:-Wl,--wrap=memalign>
  $<$<CONFIG:Debug>:-Wl,--wrap=valloc>
  $<$<CONFIG:Debug>:-Wl,--wrap=strdup>
  $<$<CONFIG:Debug>:-Wl,--wrap=free>
  $<$<CONFIG:Debug>:-Wl,--wrap=glDrawArrays>
  $<$<CONFIG:Debug>:-Wl,--wrap=glDrawElements>
  $<$<CONFIG:Debug>:-Wl,--wrap=glUniformMatrix4fv>
  $<$<CONFIG:Debug>:-Wl,--wrap=sceGxmDraw>
  $<$<CONFIG:Debug>:-Wl,--wrap=sceGxmBeginScene>
  $<$<CONFIG:Debug>:-Wl,--wrap=sceGxmEndScene>
  $<$<CONFIG:Debug>:-Wl,--wrap=sceGxmDisplayQueueAddEntry>
  $<$<CONFIG:Debug>:-Wl,--wrap=sceGxmSetVertexProgram>
  $<$<CONFIG:Debug>:-Wl,--wrap=sceGxmSetFragmentProgram>
  $<$<CONFIG:Debug>:-Wl,--wrap=sceGxmSetVertexStream>
)
target_link_libraries(${PROJECT_NAME}
  mc2_compat
  so_util
  falso_jni
  "${MC2_LOADER_DIR}/lib/kubridge/libkubridge_stub.a"
  SceIofilemgr_stub
  SceKernelModulemgr_stub
  SceKernelThreadMgr_stub
  SceVshBridge_stub
  SceFios2_stub
  -Wl,--whole-archive pthread -Wl,--no-whole-archive
  OpenSLES
  sndfile
  mp3lame
  mpg123
  opus
  vorbis
  ogg
  vorbisenc
  vorbisfile
  FLAC
  stdc++
  rio_vitaGL
  vitashark
  SceShaccCgExt
  taihen_stub
  SceShaccCg_stub
  z
  m
  ScePgf_stub
  mathneon
  SceHid_stub
  SceTouch_stub
  SceMotion_stub
  SceAppMgr_stub
  SceAppUtil_stub
  SceAvPlayer_stub
  SceAudio_stub
  SceAudioIn_stub
  SceCommonDialog_stub
  SceDisplay_stub
  SceKernelDmacMgr_stub
  SceCtrl_stub
  SceGxm_stub
  ScePower_stub
  SceSysmodule_stub
)

vita_create_self(eboot.bin ${PROJECT_NAME} UNSAFE NOASLR)
vita_create_vpk(${VITA_VPK_NAME}.vpk ${VITA_TITLEID} eboot.bin
  VERSION ${VITA_VERSION}
  NAME ${VITA_APP_NAME}
  FILE
    ${MC2_LOADER_DIR}/extras/cpuinfo cpuinfo
    ${MC2_LOADER_DIR}/extras/meminfo meminfo
    ${MC2_LOADER_DIR}/extras/urandom urandom
    ${CMAKE_CURRENT_SOURCE_DIR}/assets/livearea/icon0.png sce_sys/icon0.png
    ${CMAKE_CURRENT_SOURCE_DIR}/assets/livearea/pic0.png sce_sys/pic0.png
    ${CMAKE_CURRENT_SOURCE_DIR}/assets/livearea/startup.png sce_sys/livearea/contents/startup.png
    ${CMAKE_CURRENT_SOURCE_DIR}/assets/livearea/bg0.png sce_sys/livearea/contents/bg0.png
    ${CMAKE_CURRENT_SOURCE_DIR}/assets/livearea/template.xml sce_sys/livearea/contents/template.xml
    ${CMAKE_CURRENT_SOURCE_DIR}/assets/intro/logo-vita.mp4 logo-vita.mp4
    ${CMAKE_CURRENT_SOURCE_DIR}/assets/intro/loading.rgba loading.rgba
)
