cmake_minimum_required(VERSION 3.10)

set(TARGET_PLATFORM "t113" CACHE STRING "Target platform: t113 or rk3568")
set_property(CACHE TARGET_PLATFORM PROPERTY STRINGS t113 rk3568)

if(NOT DEFINED CMAKE_TOOLCHAIN_FILE)
    set(CMAKE_SYSTEM_NAME Linux)
    if(TARGET_PLATFORM STREQUAL "rk3568")
        set(CMAKE_SYSTEM_PROCESSOR aarch64)
        set(CMAKE_C_COMPILER aarch64-linux-gnu-gcc)
        set(CMAKE_CXX_COMPILER aarch64-linux-gnu-g++)
    else()
        set(CMAKE_SYSTEM_PROCESSOR arm)
        set(CMAKE_C_COMPILER arm-linux-gnueabihf-gcc)
        set(CMAKE_CXX_COMPILER arm-linux-gnueabihf-g++)
    endif()
endif()

project(Audio_Processing C)

set(CMAKE_C_STANDARD 99)

if(TARGET_PLATFORM STREQUAL "rk3568")
    set(DEFAULT_OPUS_ROOT "")
    set(DEFAULT_ALSA_ROOT "")
    set(DEFAULT_OPENSSL_ROOT "")
else()
    set(DEFAULT_OPUS_ROOT "/home/ubuntu/opus-1.4/install_t113")
    set(DEFAULT_ALSA_ROOT "/home/ubuntu/alsa/install_t113/usr")
    set(DEFAULT_OPENSSL_ROOT "/home/ubuntu/openssl_arm_install")
endif()

# Opus 库路径配置
set(OPUS_ROOT "${DEFAULT_OPUS_ROOT}" CACHE PATH "Opus install root")
if(OPUS_ROOT)
    set(OPUS_INCLUDE_DIR "${OPUS_ROOT}/include")
    set(OPUS_LIB_DIR "${OPUS_ROOT}/lib")
else()
    set(OPUS_INCLUDE_DIR "")
    set(OPUS_LIB_DIR "")
endif()

# ALSA 库路径配置
set(ALSA_ROOT "${DEFAULT_ALSA_ROOT}" CACHE PATH "ALSA install root")
if(ALSA_ROOT)
    set(ALSA_INCLUDE_DIR "${ALSA_ROOT}/include")
    set(ALSA_LIB_DIR "${ALSA_ROOT}/lib")
else()
    set(ALSA_INCLUDE_DIR "")
    set(ALSA_LIB_DIR "")
endif()

# 指定 OpenSSL 路径
set(OPENSSL_ROOT "${DEFAULT_OPENSSL_ROOT}" CACHE PATH "OpenSSL install root")
if(OPENSSL_ROOT)
    set(OPENSSL_INCLUDE_DIR "${OPENSSL_ROOT}/include")
    set(OPENSSL_SSL_LIBRARY "${OPENSSL_ROOT}/lib/libssl.a")
else()
    set(OPENSSL_INCLUDE_DIR "")
    set(OPENSSL_SSL_LIBRARY "")
endif()

# 查找 Opus 库
find_library(OPUS_LIB opus HINTS ${OPUS_LIB_DIR})

# 查找 ALSA 库
find_library(ALSA_LIB asound HINTS ${ALSA_LIB_DIR})

# 包含目录
include_directories(${OPUS_INCLUDE_DIR})
include_directories(${ALSA_INCLUDE_DIR})
include_directories(${WEB_INCLUDE_DIR})
include_directories(${OPENSSL_INCLUDE_DIR})
# 可执行文件
add_executable(xiaozhi_opus xiaozhi_opus.c)
# 链接库
target_link_libraries(xiaozhi_opus ${OPUS_LIB} ${ALSA_LIB} pthread m)
# 运行时库路径
set_target_properties(xiaozhi_opus PROPERTIES
    BUILD_RPATH "${OPUS_LIB_DIR}:${ALSA_LIB_DIR}"
    INSTALL_RPATH "${OPUS_LIB_DIR}:${ALSA_LIB_DIR}"
)

if(EXISTS "${CMAKE_CURRENT_SOURCE_DIR}/sound.c")
    add_executable(sound sound.c)
    target_link_libraries(sound ${OPUS_LIB} ${ALSA_LIB} pthread m)
    set_target_properties(sound PROPERTIES
        BUILD_RPATH "${OPUS_LIB_DIR}:${ALSA_LIB_DIR}"
        INSTALL_RPATH "${OPUS_LIB_DIR}:${ALSA_LIB_DIR}"
    )
else()
    message(STATUS "sound.c not found; skip sound target")
endif()
