cmake_minimum_required(VERSION 3.14)
if(NOT DEFINED CMAKE_TOOLCHAIN_FILE)
  if(NOT DEFINED ENV{VITASDK})
    message(FATAL_ERROR "Set VITASDK to the softfp VitaSDK")
  endif()
  set(CMAKE_TOOLCHAIN_FILE "$ENV{VITASDK}/share/vita.toolchain.cmake" CACHE PATH "VitaSDK toolchain")
endif()
project(gangstarvegas_vita C CXX)
include("${VITASDK}/share/vita.cmake" REQUIRED)
find_package(Python3 REQUIRED COMPONENTS Interpreter)
set(PROFILE_HEADER "${CMAKE_BINARY_DIR}/generated/game_profile.h")
add_custom_command(OUTPUT "${PROFILE_HEADER}"
  COMMAND "${Python3_EXECUTABLE}" "${CMAKE_SOURCE_DIR}/tools/game_profile.py" --header "${PROFILE_HEADER}"
  DEPENDS "${CMAKE_SOURCE_DIR}/tools/game_profile.py" VERBATIM)
set(MC2 "${CMAKE_SOURCE_DIR}/third_party/mc2-loader")
if(NOT EXISTS "${MC2}/lib/so_util/so_util.c")
  message(FATAL_ERROR "Run git submodule update --init --recursive")
endif()
add_library(so_util STATIC "${MC2}/lib/so_util/so_util.c")
target_include_directories(so_util PRIVATE include "${MC2}/lib/so_util" "${MC2}/lib/kubridge")
target_compile_options(so_util PRIVATE -O1 -mfloat-abi=softfp)
set(VGL "${CMAKE_BINARY_DIR}/vitaGL")
execute_process(COMMAND "${Python3_EXECUTABLE}" "${CMAKE_SOURCE_DIR}/tools/prepare_vitagl.py"
  --out "${VGL}" COMMAND_ERROR_IS_FATAL ANY)
file(STRINGS "${VGL}/.pinned-commit" VGL_COMMIT LIMIT_COUNT 1)
execute_process(COMMAND "${Python3_EXECUTABLE}" "${CMAKE_SOURCE_DIR}/tools/prepare_compat.py"
  --out "${CMAKE_BINARY_DIR}/compat" COMMAND_ERROR_IS_FATAL ANY)
set_property(DIRECTORY APPEND PROPERTY CMAKE_CONFIGURE_DEPENDS
  tools/prepare_compat.py tools/game_imports.txt patches/mc2-vegas.patch
  tools/prepare_vitagl.py patches/vitagl-pixel-store.patch third_party/vitaGL/Makefile third_party/vitaGL/.pinned-commit)
add_custom_target(vitaGL_build
  COMMAND make -C "${VGL}" -j4
    "CC=${CMAKE_SOURCE_DIR}/scripts/arm-vita-eabi-gcc"
    "CXX=${CMAKE_SOURCE_DIR}/scripts/arm-vita-eabi-g++"
    "AR=${CMAKE_SOURCE_DIR}/scripts/arm-vita-eabi-gcc-ar"
    SOFTFP_ABI=1 HAVE_SHADER_CACHE=1 LOG_ERRORS=2 NO_SPLASHSCREEN=1 "VGL_GIT_HASH=${VGL_COMMIT}"
  BYPRODUCTS "${VGL}/libvitaGL.a" VERBATIM)
add_library(vegas_vitaGL STATIC IMPORTED)
set_target_properties(vegas_vitaGL PROPERTIES IMPORTED_LOCATION "${VGL}/libvitaGL.a")
add_library(mc2_compat STATIC
  "${CMAKE_BINARY_DIR}/compat/dynlib.c"
  "${CMAKE_BINARY_DIR}/compat/reimpl/io.c"
  "${CMAKE_BINARY_DIR}/compat/reimpl/pthr.c"
  "${CMAKE_BINARY_DIR}/compat/reimpl/egl.c"
  "${CMAKE_BINARY_DIR}/compat/reimpl/asset_manager.cpp"
  "${MC2}/source/reimpl/bits/_ctype.c"
  "${MC2}/source/reimpl/errno.c"
  "${MC2}/source/reimpl/log.c"
  "${MC2}/source/reimpl/sys.c"
  "${MC2}/source/reimpl/time64.c"
  "${CMAKE_BINARY_DIR}/compat/utils/glutil.c"
  "${MC2}/source/utils/logger.c"
  "${MC2}/source/utils/utils.c"
  "${MC2}/lib/sha1/sha1.c")
target_include_directories(mc2_compat PRIVATE "${MC2}/lib" "${MC2}/source" "${VGL}/source")
target_compile_definitions(mc2_compat PRIVATE _GNU_SOURCE __POSIX_VISIBLE=999999
  USE_GLSL_SHADERS DATA_PATH="ux0:data/gangstarvegas/")
target_compile_options(mc2_compat PRIVATE -O1 -g3 -mfloat-abi=softfp -ffunction-sections -fdata-sections
  $<$<COMPILE_LANGUAGE:C>:-std=gnu11;-Wno-deprecated-declarations>
  $<$<COMPILE_LANGUAGE:CXX>:-std=gnu++20;-Wno-write-strings;-Wno-psabi>)
add_library(falso_jni STATIC
  "${CMAKE_BINARY_DIR}/compat/falso_jni/FalsoJNI.c" "${CMAKE_BINARY_DIR}/compat/falso_jni/FalsoJNI_ImplBridge.c"
  "${MC2}/lib/falso_jni/FalsoJNI_Logger.c" "${MC2}/lib/falso_jni/converter.c")
target_include_directories(falso_jni PRIVATE "${MC2}/lib/falso_jni")
target_compile_options(falso_jni PRIVATE -O1 -g3 -mfloat-abi=softfp -w)
add_executable(${PROJECT_NAME} source/main.c source/game_file.c source/elf_diagnostics.c
  source/diagnostics.c source/dialog.c source/sha256.c source/compat.c source/epoll.c source/offline.c
  source/bundle.c source/md5.c source/memory.c source/java.c source/audio.c source/runtime.c "${PROFILE_HEADER}")
target_include_directories(${PROJECT_NAME} PRIVATE include "${CMAKE_BINARY_DIR}/generated"
  "${MC2}/lib/so_util" "${MC2}/lib/kubridge" "${MC2}/lib" "${MC2}/source" "${VGL}/source")
add_dependencies(${PROJECT_NAME} vitaGL_build)
target_compile_definitions(${PROJECT_NAME} PRIVATE _GNU_SOURCE __POSIX_VISIBLE=999999)
target_compile_features(${PROJECT_NAME} PRIVATE c_std_11)
target_compile_options(${PROJECT_NAME} PRIVATE -O1 -g3 -Wall -Wextra -Werror -mfloat-abi=softfp)
target_link_options(${PROJECT_NAME} PRIVATE -mfloat-abi=softfp -Wl,--gc-sections)
target_link_libraries(${PROJECT_NAME}
  -Wl,--start-group mc2_compat so_util falso_jni
  "${MC2}/lib/kubridge/libkubridge_stub.a"
  -Wl,--whole-archive pthread -Wl,--no-whole-archive
  OpenSLES sndfile mp3lame mpg123 opus vorbis ogg vorbisenc vorbisfile FLAC
  stdc++ vegas_vitaGL vitashark SceShaccCgExt taihen_stub SceShaccCg_stub z m mathneon
  SceIofilemgr_stub SceKernelModulemgr_stub SceKernelThreadMgr_stub SceVshBridge_stub
  SceFios2_stub ScePgf_stub SceHid_stub SceTouch_stub SceMotion_stub SceAppMgr_stub
  SceAppUtil_stub SceAvPlayer_stub SceAudio_stub SceAudioIn_stub SceCommonDialog_stub
  SceDisplay_stub SceKernelDmacMgr_stub SceCtrl_stub SceGxm_stub ScePower_stub
  SceSysmodule_stub SceNet_stub SceNetCtl_stub -Wl,--end-group)

set(VITA_MKSFOEX_FLAGS "-d ATTRIBUTE2=12")
vita_create_self(eboot.bin ${PROJECT_NAME} UNSAFE NOASLR)
vita_create_vpk(gangstarvegas-vita.vpk GSGV00001 eboot.bin VERSION "00.05" NAME "Gangstar Vegas Vita"
  FILE
    "${MC2}/extras/cpuinfo" cpuinfo
    "${MC2}/extras/meminfo" meminfo
    "${MC2}/extras/urandom" urandom
    assets/livearea/icon0.png sce_sys/icon0.png
    assets/livearea/pic0.png sce_sys/pic0.png
    assets/livearea/bg0.png sce_sys/livearea/contents/bg0.png
    assets/livearea/startup.png sce_sys/livearea/contents/startup.png
    assets/livearea/template.xml sce_sys/livearea/contents/template.xml)
