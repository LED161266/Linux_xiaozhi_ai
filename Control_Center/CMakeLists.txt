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

project(xiaozhi_http C)

# 设置 C 标准
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
    set(DEFAULT_CURL_ROOT "")
    set(DEFAULT_OPENSSL_ROOT "")
    set(DEFAULT_OPUS_ROOT "")
    set(DEFAULT_ALSA_ROOT "")
    set(DEFAULT_LIBWEBSOCKETS_ROOT "")
    set(DEFAULT_ZLIB_LIB_DIR "")
    set(DEFAULT_CJSON_ROOT "")
else()
    set(DEFAULT_CURL_ROOT "/home/ubuntu/curl/curl-7.71.1/_install_arm")
    set(DEFAULT_OPENSSL_ROOT "/home/ubuntu/openssl-1.1.1w/openssl_arm_install")
    set(DEFAULT_OPUS_ROOT "/home/ubuntu/opus-1.4/install_t113")
    set(DEFAULT_ALSA_ROOT "/home/ubuntu/alsa/install_t113/usr")
    set(DEFAULT_LIBWEBSOCKETS_ROOT "/home/ubuntu/websocket/libwebsockets/build/install_t113")
    set(DEFAULT_ZLIB_LIB_DIR "/home/ubuntu/t113/t113-linux/out/t113/evb1_auto/buildroot/buildroot/target/usr/lib")
    set(DEFAULT_CJSON_ROOT "/home/ubuntu/cjson/cJSON-1.7.10/install_t113")
endif()

# 指定自定义 libcurl 路径
set(RK3568_INCLUDE_DIRS "")
if(USE_RK3568_SYSROOT)
    set(RK3568_INCLUDE_DIRS
        "${RK3568_SYSROOT}/usr/include"
        "${RK3568_SYSROOT}/usr/include/aarch64-linux-gnu"
    )
    set(RK3568_LIBRARY_DIRS
        "${RK3568_SYSROOT}/lib/aarch64-linux-gnu"
        "${RK3568_SYSROOT}/usr/lib/aarch64-linux-gnu"
    )

    find_path(CURL_INCLUDE_DIR NAMES curl/curl.h PATHS ${RK3568_INCLUDE_DIRS} NO_DEFAULT_PATH)
    find_library(CURL_LIBRARY NAMES curl PATHS ${RK3568_LIBRARY_DIRS} NO_DEFAULT_PATH)

    find_path(OPENSSL_INCLUDE_DIR NAMES openssl/ssl.h PATHS ${RK3568_INCLUDE_DIRS} NO_DEFAULT_PATH)
    find_library(OPENSSL_SSL_LIBRARY NAMES ssl PATHS ${RK3568_LIBRARY_DIRS} NO_DEFAULT_PATH)
    find_library(OPENSSL_CRYPTO_LIBRARY NAMES crypto PATHS ${RK3568_LIBRARY_DIRS} NO_DEFAULT_PATH)

    find_path(OPUS_INCLUDE_DIR NAMES opus/opus.h PATHS ${RK3568_INCLUDE_DIRS} NO_DEFAULT_PATH)
    find_library(OPUS_LIB NAMES opus PATHS ${RK3568_LIBRARY_DIRS} NO_DEFAULT_PATH)

    find_path(ALSA_INCLUDE_DIR NAMES alsa/asoundlib.h PATHS ${RK3568_INCLUDE_DIRS} NO_DEFAULT_PATH)
    find_library(ALSA_LIB NAMES asound PATHS ${RK3568_LIBRARY_DIRS} NO_DEFAULT_PATH)

    find_path(LIBWEBSOCKETS_INCLUDE_DIR NAMES libwebsockets.h PATHS ${RK3568_INCLUDE_DIRS} NO_DEFAULT_PATH)
    find_library(WEBSOCKETS_LIB NAMES websockets PATHS ${RK3568_LIBRARY_DIRS} NO_DEFAULT_PATH)

    find_path(CJSON_INCLUDE_DIR NAMES cjson/cJSON.h cJSON.h PATHS ${RK3568_INCLUDE_DIRS} NO_DEFAULT_PATH)
    find_library(CJSON_LIB NAMES cjson PATHS ${RK3568_LIBRARY_DIRS} NO_DEFAULT_PATH)

    find_library(ZLIB_LIB NAMES z PATHS ${RK3568_LIBRARY_DIRS} NO_DEFAULT_PATH)
    file(GLOB RK3568_ZLIB_CANDIDATES
        "${RK3568_SYSROOT}/lib/aarch64-linux-gnu/libz.so.*"
        "${RK3568_SYSROOT}/usr/lib/aarch64-linux-gnu/libz.so.*"
    )
    foreach(RK3568_ZLIB_CANDIDATE IN LISTS RK3568_ZLIB_CANDIDATES)
        if(EXISTS "${RK3568_ZLIB_CANDIDATE}" AND NOT IS_SYMLINK "${RK3568_ZLIB_CANDIDATE}")
            set(ZLIB_LIB "${RK3568_ZLIB_CANDIDATE}")
            break()
        endif()
    endforeach()

    foreach(REQUIRED_LIB
        CURL_INCLUDE_DIR CURL_LIBRARY
        OPENSSL_INCLUDE_DIR OPENSSL_SSL_LIBRARY OPENSSL_CRYPTO_LIBRARY
        OPUS_INCLUDE_DIR OPUS_LIB
        ALSA_INCLUDE_DIR ALSA_LIB
        LIBWEBSOCKETS_INCLUDE_DIR WEBSOCKETS_LIB
        CJSON_INCLUDE_DIR CJSON_LIB
        ZLIB_LIB
    )
        if(NOT DEFINED ${REQUIRED_LIB} OR "${${REQUIRED_LIB}}" STREQUAL "" OR "${${REQUIRED_LIB}}" MATCHES "-NOTFOUND$")
            message(FATAL_ERROR "RK3568 dependency ${REQUIRED_LIB} not found in ${RK3568_SYSROOT}")
        endif()
    endforeach()
else()

set(CURL_ROOT "${DEFAULT_CURL_ROOT}" CACHE PATH "curl install root")
if(CURL_ROOT)
    set(CURL_INCLUDE_DIR "${CURL_ROOT}/include")
    set(CURL_LIBRARY "${CURL_ROOT}/lib/libcurl.a")
else()
    set(CURL_INCLUDE_DIR "")
    find_library(CURL_LIBRARY curl)
endif()

# 指定 OpenSSL 路径
set(OPENSSL_ROOT "${DEFAULT_OPENSSL_ROOT}" CACHE PATH "OpenSSL install root")
if(OPENSSL_ROOT)
    set(OPENSSL_INCLUDE_DIR "${OPENSSL_ROOT}/include")
    set(OPENSSL_SSL_LIBRARY "${OPENSSL_ROOT}/lib/libssl.a")
    set(OPENSSL_CRYPTO_LIBRARY "${OPENSSL_ROOT}/lib/libcrypto.a")
else()
    set(OPENSSL_INCLUDE_DIR "")
    find_library(OPENSSL_SSL_LIBRARY ssl)
    find_library(OPENSSL_CRYPTO_LIBRARY crypto)
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

# 指定 libwebsockets 路径
set(LIBWEBSOCKETS_ROOT "${DEFAULT_LIBWEBSOCKETS_ROOT}" CACHE PATH "libwebsockets install root")
if(LIBWEBSOCKETS_ROOT)
    set(LIBWEBSOCKETS_INCLUDE_DIR "${LIBWEBSOCKETS_ROOT}/include")
    set(LIBWEBSOCKETS_LIB_DIR "${LIBWEBSOCKETS_ROOT}/lib")
else()
    set(LIBWEBSOCKETS_INCLUDE_DIR "")
    set(LIBWEBSOCKETS_LIB_DIR "")
endif()

# 指定 zlib 路径（修复：去掉多余的 /usr）
set(ZLIB_LIB_DIR "${DEFAULT_ZLIB_LIB_DIR}" CACHE PATH "zlib library directory")

# 指定cjson 库路径
set(CJSON_ROOT "${DEFAULT_CJSON_ROOT}" CACHE PATH "cJSON install root")
if(CJSON_ROOT)
    set(CJSON_INCLUDE_DIR "${CJSON_ROOT}/include")
    set(CJSON_LIB_DIR "${CJSON_ROOT}/lib")
else()
    set(CJSON_INCLUDE_DIR "")
    set(CJSON_LIB_DIR "")
endif()

# 修改：使用动态库而非静态库
if(CJSON_LIB_DIR)
    set(CJSON_LIB "${CJSON_LIB_DIR}/libcjson.so")
else()
    find_library(CJSON_LIB cjson)
endif()
# 设置可执行文件输出目录
endif()
set(CMAKE_RUNTIME_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR})

# 添加可执行文件
add_executable(xiaozhi_http xiaozhi_http.c)

# 添加 websocket 测试可执行文件
add_executable(websocket_test websocket_test.c)
set(WEBSOCKET_TARGETS websocket_test)
if(EXISTS "${CMAKE_CURRENT_SOURCE_DIR}/websocket.c")
    add_executable(websocket websocket.c)
    list(APPEND WEBSOCKET_TARGETS websocket)
else()
    message(STATUS "websocket.c not found; skip websocket target")
endif()
# 包含头文件路径
target_include_directories(xiaozhi_http PRIVATE 
    ${RK3568_INCLUDE_DIRS}
    ${CURL_INCLUDE_DIR}
    ${OPENSSL_INCLUDE_DIR}
    ${OPUS_INCLUDE_DIR}
    ${ALSA_INCLUDE_DIR}
)

# websocket_test 包含头文件路径
target_include_directories(websocket_test PRIVATE 
    ${RK3568_INCLUDE_DIRS}
    ${LIBWEBSOCKETS_INCLUDE_DIR}
    ${OPENSSL_INCLUDE_DIR}
    ${CJSON_INCLUDE_DIR}
)

if(TARGET websocket)
    target_include_directories(websocket PRIVATE
        ${RK3568_INCLUDE_DIRS}
        ${LIBWEBSOCKETS_INCLUDE_DIR}
        ${OPENSSL_INCLUDE_DIR}
        ${CJSON_INCLUDE_DIR}
    )
endif()

# 查找库文件
find_library(OPUS_LIB opus HINTS ${OPUS_LIB_DIR})
find_library(ALSA_LIB asound HINTS ${ALSA_LIB_DIR})

# 直接使用 zlib 库路径（避免 find_library 找到系统库）
if(ZLIB_LIB_DIR)
    set(ZLIB_LIB "${ZLIB_LIB_DIR}/libz.so")
else()
    find_library(ZLIB_LIB z)
endif()

# 使用动态库
if(LIBWEBSOCKETS_LIB_DIR)
    set(WEBSOCKETS_LIB "${LIBWEBSOCKETS_LIB_DIR}/libwebsockets.so")
else()
    find_library(WEBSOCKETS_LIB websockets)
endif()

# 链接库及依赖
target_link_libraries(xiaozhi_http 
    ${CURL_LIBRARY}
    ${OPENSSL_SSL_LIBRARY}
    ${OPENSSL_CRYPTO_LIBRARY}
    ${OPUS_LIB}
    ${ALSA_LIB}
    pthread
    dl
)

# websocket_test 链接库及依赖（使用 start-group 处理循环依赖）
target_link_libraries(websocket_test 
    -Wl,--start-group
    ${WEBSOCKETS_LIB}
    ${OPENSSL_SSL_LIBRARY}
    ${OPENSSL_CRYPTO_LIBRARY}
    -Wl,--end-group
    pthread
    dl
    ${ZLIB_LIB}
    ${CJSON_LIB}
)

if(TARGET websocket)
    target_link_libraries(websocket
        -Wl,--start-group
        ${WEBSOCKETS_LIB}
        ${OPENSSL_SSL_LIBRARY}
        ${OPENSSL_CRYPTO_LIBRARY}
        -Wl,--end-group
        pthread
        dl
        ${ZLIB_LIB}
        ${CJSON_LIB}
    )
endif()

# 添加库搜索路径（CMake 3.10 兼容方式）
if(NOT USE_RK3568_SYSROOT)
    set_target_properties(websocket_test PROPERTIES
        LINK_FLAGS "-L${OPENSSL_ROOT}/lib -L${LIBWEBSOCKETS_LIB_DIR} -L${ZLIB_LIB_DIR}"
        LINK_FLAGS "-L${OPENSSL_ROOT}/lib -L${LIBWEBSOCKETS_LIB_DIR} -L${ZLIB_LIB_DIR} -L${CJSON_LIB_DIR}"
    )

    if(TARGET websocket)
        set_target_properties(websocket PROPERTIES
            LINK_FLAGS "-L${OPENSSL_ROOT}/lib -L${LIBWEBSOCKETS_LIB_DIR} -L${ZLIB_LIB_DIR}"
            LINK_FLAGS "-L${OPENSSL_ROOT}/lib -L${LIBWEBSOCKETS_LIB_DIR} -L${ZLIB_LIB_DIR} -L${CJSON_LIB_DIR}"
        )
    endif()
endif()

# 设置编译选项
target_compile_options(xiaozhi_http PRIVATE -Wall -Wextra)
target_compile_options(websocket_test PRIVATE -Wall -Wextra)
if(TARGET websocket)
    target_compile_options(websocket PRIVATE -Wall -Wextra)
endif()
# 安装规则
install(TARGETS xiaozhi_http DESTINATION bin)
install(TARGETS ${WEBSOCKET_TARGETS} DESTINATION bin)
