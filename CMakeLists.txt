cmake_minimum_required(VERSION 3.16)

if(NOT DEFINED CMAKE_TOOLCHAIN_FILE)
  if(DEFINED ENV{VITASDK})
    set(CMAKE_TOOLCHAIN_FILE "$ENV{VITASDK}/share/vita.toolchain.cmake" CACHE PATH "toolchain file")
  else()
    message(FATAL_ERROR "Please define VITASDK to point to your SDK path!")
  endif()
endif()

project(VPKGJ CXX C)
include("$ENV{VITASDK}/share/vita.cmake" REQUIRED)

set(VITA_APP_NAME "VPKGJ")
set(VITA_TITLEID "VPKG00001")
set(VITA_VERSION "01.90")
set(VITA_MKSFOEX_FLAGS "${VITA_MKSFOEX_FLAGS} -d ATTRIBUTE2=12")

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_C_FLAGS "${CMAKE_C_FLAGS} -O2 -Wall")
set(CMAKE_CXX_FLAGS "${CMAKE_CXX_FLAGS} -O2 -Wall -Wno-psabi")

add_executable(vpkgj
  src/main.cpp
  src/backup.cpp
  src/bgdl.cpp
  src/catalog.cpp
  src/gfx.cpp
  src/headbin.cpp
  src/images.cpp
  src/ime.cpp
  src/net.cpp
  src/pkg.cpp
  src/promote.cpp
  src/shaccg.cpp
  src/tasks.cpp
  src/util.cpp
  src/zip.cpp
)

target_include_directories(vpkgj PRIVATE $ENV{VITASDK}/arm-vita-eabi/include/jsoncpp)
target_link_options(vpkgj PRIVATE -Wl,--wrap,fcntl -Wl,--wrap,getaddrinfo -Wl,--wrap,freeaddrinfo)

target_link_libraries(vpkgj
  vita2d
  freetype
  png
  turbojpeg
  jpeg
  jsoncpp
  minizip
  ppmd
  curl
  mbedtls
  mbedx509
  mbedcrypto
  ssl
  crypto
  bz2
  lzma
  zstd
  z
  m
  pthread
  SceAppMgr_stub
  SceAppUtil_stub
  SceCommonDialog_stub
  SceCtrl_stub
  SceDisplay_stub
  SceGxm_stub
  SceIme_stub
  SceNet_stub
  SceNetCtl_stub
  ScePgf_stub
  ScePower_stub
  ScePromoterUtil_stub
  SceShellSvc_stub
  SceSysmodule_stub
  SceTouch_stub
  SceRtc_stub
  SceKernelThreadMgr_stub
  SceKernelModulemgr_stub
  SceSysmem_stub
  SceIofilemgr_stub
  SceProcessmgr_stub
  SceLibKernel_stub
  taihen_stub
)

vita_create_self(eboot.bin vpkgj UNSAFE)

file(GLOB MBEDTLS_SRC ${CMAKE_SOURCE_DIR}/daemon/mbedtls/library/*.c)
add_executable(vpkgj_daemon daemon/main.c daemon/dnet.c ${MBEDTLS_SRC})
target_include_directories(vpkgj_daemon PRIVATE ${CMAKE_SOURCE_DIR}/daemon ${CMAKE_SOURCE_DIR}/daemon/mbedtls/include ${CMAKE_SOURCE_DIR}/daemon/mbedtls/library)
target_compile_definitions(vpkgj_daemon PRIVATE MBEDTLS_CONFIG_FILE="tls_config.h")
target_compile_options(vpkgj_daemon PRIVATE -Os -fno-builtin)
target_link_libraries(vpkgj_daemon
  taihen_stub
  taipool
  k
  gcc
  SceLibKernel_stub_weak
  SceKernelThreadMgr_stub
  SceKernelModulemgr_stub
  SceSysmem_stub
  SceAppMgr_stub
  SceSysmodule_stub
  SceNet_stub_weak
  SceNetCtl_stub_weak
  SceUdcd_stub_weak
  SceIofilemgr_stub
  SceLibc_stub_weak
)
set_target_properties(vpkgj_daemon PROPERTIES LINK_FLAGS "-nostdlib")
add_executable(vpkgj_psm_kernel psm/kernel/main.c)
target_link_libraries(vpkgj_psm_kernel
  k
  gcc
  SceSysmemForDriver_stub
  SceSysclibForDriver_stub
  SceFios2KernelForDriver_stub
  SceProcEventForDriver_stub
  taihenForKernel_stub
)
set_target_properties(vpkgj_psm_kernel PROPERTIES LINK_FLAGS "-nostdlib")
vita_create_self(vpkgj_psm.skprx vpkgj_psm_kernel UNSAFE CONFIG ${CMAKE_SOURCE_DIR}/psm/kernel/exports.yml)

add_executable(vpkgj_psm_user psm/user/main.c)
target_link_libraries(vpkgj_psm_user
  taihen_stub
  k
  gcc
  SceLibKernel_stub_weak
  SceAppMgr_stub
  SceLibc_stub_weak
)
set_target_properties(vpkgj_psm_user PROPERTIES LINK_FLAGS "-nostdlib")
target_compile_options(vpkgj_psm_user PRIVATE -std=gnu11)
vita_create_self(vpkgj_psm.suprx vpkgj_psm_user UNSAFE CONFIG ${CMAKE_SOURCE_DIR}/psm/user/exports.yml)

vita_create_self(vpkgj_daemon.suprx vpkgj_daemon UNSAFE CONFIG ${CMAKE_SOURCE_DIR}/daemon/exports.yml)

vita_create_vpk(${PROJECT_NAME}.vpk ${VITA_TITLEID} eboot.bin
  VERSION ${VITA_VERSION}
  NAME ${VITA_APP_NAME}
  FILE sce_sys/icon0.png sce_sys/icon0.png
       sce_sys/livearea/contents/bg.png sce_sys/livearea/contents/bg.png
       sce_sys/livearea/contents/startup.png sce_sys/livearea/contents/startup.png
       sce_sys/livearea/contents/template.xml sce_sys/livearea/contents/template.xml
       assets/Inter-Regular.ttf assets/Inter-Regular.ttf
       assets/Inter-SemiBold.ttf assets/Inter-SemiBold.ttf
       assets/mascot.png assets/mascot.png
       assets/logo.png assets/logo.png
       ${CMAKE_BINARY_DIR}/vpkgj_daemon.suprx vpkgj_daemon.suprx
       ${CMAKE_BINARY_DIR}/vpkgj_psm.skprx vpkgj_psm.skprx
       ${CMAKE_BINARY_DIR}/vpkgj_psm.suprx vpkgj_psm.suprx
)

set(VPKGJ_COVERS "${CMAKE_SOURCE_DIR}/../db" CACHE PATH "vpkgj-db checkout with media/covers and covers.json")
if(EXISTS "${VPKGJ_COVERS}/covers.json" AND EXISTS "${VPKGJ_COVERS}/media/covers")
  add_custom_command(TARGET ${PROJECT_NAME}.vpk-vpk POST_BUILD
    COMMAND ${CMAKE_COMMAND} -E rm -rf "${CMAKE_BINARY_DIR}/bundle"
    COMMAND ${CMAKE_COMMAND} -E make_directory "${CMAKE_BINARY_DIR}/bundle/covers"
    COMMAND ${CMAKE_COMMAND} -E copy "${VPKGJ_COVERS}/covers.json" "${CMAKE_BINARY_DIR}/bundle/covers.json"
    COMMAND sh -c "cd '${VPKGJ_COVERS}/media' && find covers -name c.jpg | while read f; do mkdir -p '${CMAKE_BINARY_DIR}/bundle/'$(dirname $f) && cp $f '${CMAKE_BINARY_DIR}/bundle/'$f; done"
    COMMAND sh -c "cd '${CMAKE_BINARY_DIR}/bundle' && rm -f covers.pak && zip -q -0 -r covers.pak covers && zip -q -0 '${CMAKE_BINARY_DIR}/${PROJECT_NAME}.vpk' covers.pak covers.json"
    VERBATIM)
endif()
