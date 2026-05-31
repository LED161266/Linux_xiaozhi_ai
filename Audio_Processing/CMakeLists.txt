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
set(CMAKE_C_STANDARD_REQUIRED ON)

set(USE_RK3568_SYSROOT OFF)
if(DEFINED RK3568_SYSROOT AND RK3568_SYSROOT)
    set(USE_RK3568_SYSROOT ON)
elseif(DEFINED ENV{RK3568_SYSROOT} AND NOT "$ENV{RK3568_SYSROOT}" STREQUAL "")
    set(RK3568_SYSROOT "$ENV{RK3568_SYSROOT}" CACHE PATH "RK3568 sysroot")
    set(USE_RK3568_SYSROOT ON)
elseif(CMAKE_FIND_ROOT_PATH MATCHES "rk3568-sysroot")
    foreach(ROOT_PATH IN LISTS CMAKE_FIND_ROOT_PATH)
        if(ROOT_PATH MATCHES "rk3568-sysroot")
            set(RK3568_SYSROOT "${ROOT_PATH}" CACHE PATH "RK3568 sysroot")
            set(USE_RK3568_SYSROOT ON)
            break()
        endif()
    endforeach()
endif()

if(TARGET_PLATFORM STREQUAL "rk3568" OR USE_RK3568_SYSROOT)
    set(DEFAULT_OPUS_ROOT "")
    set(DEFAULT_ALSA_ROOT "")
    set(DEFAULT_OPENSSL_ROOT "")
else()
    set(DEFAULT_OPUS_ROOT "/home/ubuntu/opus-1.4/install_t113")
    set(DEFAULT_ALSA_ROOT "/home/ubuntu/alsa/install_t113/usr")
    set(DEFAULT_OPENSSL_ROOT "/home/ubuntu/openssl_arm_install")
endif()

set(AUDIO_INCLUDE_DIRS "")
set(AUDIO_RPATH "")

if(USE_RK3568_SYSROOT)
    set(RK3568_INCLUDE_DIRS
        "${RK3568_SYSROOT}/usr/include"
        "${RK3568_SYSROOT}/usr/include/aarch64-linux-gnu"
    )
    set(RK3568_LIBRARY_DIRS
        "${RK3568_SYSROOT}/lib/aarch64-linux-gnu"
        "${RK3568_SYSROOT}/usr/lib/aarch64-linux-gnu"
    )

    find_path(OPUS_INCLUDE_DIR NAMES opus/opus.h PATHS ${RK3568_INCLUDE_DIRS} NO_DEFAULT_PATH)
    find_library(OPUS_LIB NAMES opus PATHS ${RK3568_LIBRARY_DIRS} NO_DEFAULT_PATH)
    file(GLOB RK3568_OPUS_CANDIDATES
        "${RK3568_SYSROOT}/lib/aarch64-linux-gnu/libopus.so.*"
        "${RK3568_SYSROOT}/usr/lib/aarch64-linux-gnu/libopus.so.*"
    )
    foreach(RK3568_OPUS_CANDIDATE IN LISTS RK3568_OPUS_CANDIDATES)
        if(EXISTS "${RK3568_OPUS_CANDIDATE}" AND NOT IS_SYMLINK "${RK3568_OPUS_CANDIDATE}")
            set(OPUS_LIB "${RK3568_OPUS_CANDIDATE}")
            break()
        endif()
    endforeach()

    find_path(ALSA_INCLUDE_DIR NAMES alsa/asoundlib.h PATHS ${RK3568_INCLUDE_DIRS} NO_DEFAULT_PATH)
    find_library(ALSA_LIB NAMES asound PATHS ${RK3568_LIBRARY_DIRS} NO_DEFAULT_PATH)

    find_path(OPENSSL_INCLUDE_DIR NAMES openssl/ssl.h PATHS ${RK3568_INCLUDE_DIRS} NO_DEFAULT_PATH)
    find_library(OPENSSL_SSL_LIBRARY NAMES ssl PATHS ${RK3568_LIBRARY_DIRS} NO_DEFAULT_PATH)
    find_library(OPENSSL_CRYPTO_LIBRARY NAMES crypto PATHS ${RK3568_LIBRARY_DIRS} NO_DEFAULT_PATH)

    foreach(REQUIRED_DEP
        OPUS_INCLUDE_DIR OPUS_LIB
        ALSA_INCLUDE_DIR ALSA_LIB
        OPENSSL_INCLUDE_DIR OPENSSL_SSL_LIBRARY OPENSSL_CRYPTO_LIBRARY
    )
        if(NOT DEFINED ${REQUIRED_DEP} OR "${${REQUIRED_DEP}}" STREQUAL "" OR "${${REQUIRED_DEP}}" MATCHES "-NOTFOUND$")
            message(FATAL_ERROR "RK3568 dependency ${REQUIRED_DEP} not found in ${RK3568_SYSROOT}")
        endif()
    endforeach()

    set(AUDIO_INCLUDE_DIRS
        ${RK3568_INCLUDE_DIRS}
        ${OPUS_INCLUDE_DIR}
        ${ALSA_INCLUDE_DIR}
        ${OPENSSL_INCLUDE_DIR}
    )
else()
    set(OPUS_ROOT "${DEFAULT_OPUS_ROOT}" CACHE PATH "Opus install root")
    if(OPUS_ROOT)
        set(OPUS_INCLUDE_DIR "${OPUS_ROOT}/include")
        set(OPUS_LIB_DIR "${OPUS_ROOT}/lib")
    else()
        set(OPUS_INCLUDE_DIR "")
        set(OPUS_LIB_DIR "")
    endif()

    set(ALSA_ROOT "${DEFAULT_ALSA_ROOT}" CACHE PATH "ALSA install root")
    if(ALSA_ROOT)
        set(ALSA_INCLUDE_DIR "${ALSA_ROOT}/include")
        set(ALSA_LIB_DIR "${ALSA_ROOT}/lib")
    else()
        set(ALSA_INCLUDE_DIR "")
        set(ALSA_LIB_DIR "")
    endif()

    set(OPENSSL_ROOT "${DEFAULT_OPENSSL_ROOT}" CACHE PATH "OpenSSL install root")
    if(OPENSSL_ROOT)
        set(OPENSSL_INCLUDE_DIR "${OPENSSL_ROOT}/include")
        set(OPENSSL_SSL_LIBRARY "${OPENSSL_ROOT}/lib/libssl.a")
    else()
        set(OPENSSL_INCLUDE_DIR "")
        set(OPENSSL_SSL_LIBRARY "")
    endif()

    find_library(OPUS_LIB opus HINTS ${OPUS_LIB_DIR})
    find_library(ALSA_LIB asound HINTS ${ALSA_LIB_DIR})

    set(AUDIO_INCLUDE_DIRS
        ${OPUS_INCLUDE_DIR}
        ${ALSA_INCLUDE_DIR}
        ${WEB_INCLUDE_DIR}
        ${OPENSSL_INCLUDE_DIR}
    )
    set(AUDIO_RPATH "${OPUS_LIB_DIR}:${ALSA_LIB_DIR}")
endif()

add_executable(xiaozhi_opus xiaozhi_opus.c)
target_include_directories(xiaozhi_opus PRIVATE ${AUDIO_INCLUDE_DIRS})
target_link_libraries(xiaozhi_opus ${OPUS_LIB} ${ALSA_LIB} pthread m)

if(NOT USE_RK3568_SYSROOT)
    set_target_properties(xiaozhi_opus PROPERTIES
        BUILD_RPATH "${AUDIO_RPATH}"
        INSTALL_RPATH "${AUDIO_RPATH}"
    )
endif()

if(EXISTS "${CMAKE_CURRENT_SOURCE_DIR}/sound.c")
    add_executable(sound sound.c)
    target_include_directories(sound PRIVATE ${AUDIO_INCLUDE_DIRS})
    target_link_libraries(sound ${OPUS_LIB} ${ALSA_LIB} pthread m)
    if(NOT USE_RK3568_SYSROOT)
        set_target_properties(sound PROPERTIES
            BUILD_RPATH "${AUDIO_RPATH}"
            INSTALL_RPATH "${AUDIO_RPATH}"
        )
    endif()
else()
    message(STATUS "sound.c not found; skip sound target")
endif()
