cmake_minimum_required(VERSION 3.21)
project(PhoneLink VERSION 0.2.0 LANGUAGES C CXX)

if(NOT MSVC OR NOT CMAKE_SIZEOF_VOID_P EQUAL 8)
    message(FATAL_ERROR "PhoneLink builds with 64-bit MSVC (Visual Studio 2022, x64).")
endif()

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
# Static CRT so the .asi has no VC++ redistributable dependency.
set(CMAKE_MSVC_RUNTIME_LIBRARY "MultiThreaded$<$<CONFIG:Debug>:Debug>")

option(PHONELINK_DEPLOY "Copy PhoneLink.asi into GTA_DIR after building" OFF)
set(GTA_DIR "" CACHE PATH "GTA V install directory (where ScriptHookV.dll lives), used by PHONELINK_DEPLOY")
option(PHONELINK_HARNESS "Build the out-of-game test harness" ON)

find_package(Python3 REQUIRED COMPONENTS Interpreter)

set(GEN_DIR ${CMAKE_BINARY_DIR}/generated)
set(DEPS_DIR ${CMAKE_BINARY_DIR}/deps)
file(MAKE_DIRECTORY ${GEN_DIR} ${DEPS_DIR})

# ---- third-party single files, pinned by version and SHA-256 -------------------------------
function(phonelink_fetch url dest sha256)
    if(NOT EXISTS ${dest})
        message(STATUS "Downloading ${url}")
        file(DOWNLOAD ${url} ${dest} EXPECTED_HASH SHA256=${sha256} TLS_VERIFY ON STATUS status)
        list(GET status 0 code)
        if(NOT code EQUAL 0)
            file(REMOVE ${dest})
            message(FATAL_ERROR "Download failed: ${url} (${status})")
        endif()
    endif()
endfunction()

phonelink_fetch(https://raw.githubusercontent.com/mackron/miniaudio/0.11.25/miniaudio.h
    ${DEPS_DIR}/miniaudio.h ac7af4de748b7e26b777f37e01cee313a308a7296a3eb080e2906b320cc55c89)
phonelink_fetch(https://github.com/nlohmann/json/releases/download/v3.11.3/json.hpp
    ${DEPS_DIR}/json.hpp 9bea4c8066ef4a1c206b2be5a36302f8926f7fdc6087af5d20b417d0cf103ea6)
# alloc8or/gta5-nativedb-data, pinned commit
phonelink_fetch(https://raw.githubusercontent.com/alloc8or/gta5-nativedb-data/424fb51b089049a9fbcebcc641500b1d44d255b4/natives.json
    ${DEPS_DIR}/natives.json d13c7e8d627dfe4eab11ba1c25b3621562ceebfe68b0fb5cfc0644d47469b441)

include(FetchContent)
set(USE_TLS OFF CACHE BOOL "" FORCE)
set(USE_ZLIB OFF CACHE BOOL "" FORCE)
set(IXWEBSOCKET_INSTALL OFF CACHE BOOL "" FORCE)
FetchContent_Declare(ixwebsocket
    GIT_REPOSITORY https://github.com/machinezone/IXWebSocket.git
    GIT_TAG v11.4.5
    GIT_SHALLOW TRUE)
FetchContent_MakeAvailable(ixwebsocket)

# ---- generated: natives.h (from the native DB) and the ScriptHookV import library -------------
add_custom_command(
    OUTPUT ${GEN_DIR}/natives.h
    COMMAND Python3::Interpreter ${CMAKE_CURRENT_SOURCE_DIR}/tools/gen_natives.py ${DEPS_DIR}/natives.json ${GEN_DIR}/natives.h
    DEPENDS ${CMAKE_CURRENT_SOURCE_DIR}/tools/gen_natives.py ${DEPS_DIR}/natives.json
    COMMENT "Generating natives.h")

set(SHV_LIB ${GEN_DIR}/ScriptHookV.lib)
add_custom_command(
    OUTPUT ${SHV_LIB}
    COMMAND ${CMAKE_AR} /nologo /def:${CMAKE_CURRENT_SOURCE_DIR}/sdk/ScriptHookV.def /machine:x64 /out:${SHV_LIB}
    DEPENDS ${CMAKE_CURRENT_SOURCE_DIR}/sdk/ScriptHookV.def
    COMMENT "Generating ScriptHookV import library")
add_custom_target(phonelink_generated DEPENDS ${GEN_DIR}/natives.h ${SHV_LIB})

# ---- the plugin ------------------------------------------------------------------------------------
add_library(PhoneLink SHARED
    src/main.cpp
    src/Script.cpp
    src/ApiServer.cpp
    src/Audio.cpp
    src/Tts.cpp
    src/Config.cpp
    src/Log.cpp
    src/Base64.cpp
    src/Watchdog.cpp
    src/NativePhone.cpp
    ${GEN_DIR}/natives.h
)
add_dependencies(PhoneLink phonelink_generated)
set_target_properties(PhoneLink PROPERTIES SUFFIX ".asi")
target_include_directories(PhoneLink PRIVATE sdk/inc ${GEN_DIR} ${DEPS_DIR})
target_compile_definitions(PhoneLink PRIVATE WIN32_LEAN_AND_MEAN NOMINMAX _CRT_SECURE_NO_WARNINGS
    PHONELINK_VERSION="${PROJECT_VERSION}")
target_compile_options(PhoneLink PRIVATE /W3 /utf-8 /bigobj)
target_link_libraries(PhoneLink PRIVATE ${SHV_LIB} ixwebsocket ole32 sapi ws2_32)

if(PHONELINK_DEPLOY)
    if(NOT GTA_DIR)
        message(FATAL_ERROR "PHONELINK_DEPLOY needs GTA_DIR, e.g. -DGTA_DIR=\"C:/Program Files/Rockstar Games/Grand Theft Auto V\"")
    endif()
    add_custom_command(TARGET PhoneLink POST_BUILD
        COMMAND ${CMAKE_COMMAND} -E copy_if_different $<TARGET_FILE:PhoneLink> "${GTA_DIR}/"
        COMMAND ${CMAKE_COMMAND} -DSRC=${CMAKE_CURRENT_SOURCE_DIR}/PhoneLink.ini -DDST=${GTA_DIR}
                -P ${CMAKE_CURRENT_SOURCE_DIR}/cmake/copy_if_missing.cmake
        COMMENT "Deploying PhoneLink.asi to ${GTA_DIR}")
endif()

# ---- out-of-game test harness: a stub ScriptHookV.dll + host exe that runs the plugin ----------------
if(PHONELINK_HARNESS)
    set(HARNESS_DIR ${CMAKE_BINARY_DIR}/harness)
    add_library(ScriptHookVStub SHARED tests/harness/stub_shv.cpp)
    set_target_properties(ScriptHookVStub PROPERTIES OUTPUT_NAME ScriptHookV
        RUNTIME_OUTPUT_DIRECTORY ${HARNESS_DIR} ARCHIVE_OUTPUT_DIRECTORY ${HARNESS_DIR}/lib)
    add_executable(harness tests/harness/host.cpp)
    target_link_libraries(harness PRIVATE dbghelp)
    set_target_properties(harness PROPERTIES RUNTIME_OUTPUT_DIRECTORY ${HARNESS_DIR})
    add_dependencies(harness ScriptHookVStub PhoneLink)
    add_custom_command(TARGET PhoneLink POST_BUILD
        COMMAND ${CMAKE_COMMAND} -E make_directory ${HARNESS_DIR}
        COMMAND ${CMAKE_COMMAND} -E copy_if_different $<TARGET_FILE:PhoneLink> ${HARNESS_DIR}/)
endif()

# ---- tools -------------------------------------------------------------------------------------------
add_executable(stackdump EXCLUDE_FROM_ALL tools/stackdump.cpp)
target_link_libraries(stackdump PRIVATE dbghelp)
