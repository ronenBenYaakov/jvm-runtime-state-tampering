cmake_minimum_required(VERSION 3.20)
project(gc_user CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

# 1. Normalize Windows backslashes to CMake forward slashes
if(DEFINED ENV{USERPROFILE})
    file(TO_CMAKE_PATH "$ENV{USERPROFILE}" USER_HOME)
elseif(DEFINED ENV{HOME})
    file(TO_CMAKE_PATH "$ENV{HOME}" USER_HOME)
endif()

# 2. Check JAVA_HOME first, then auto-discover any JDK under ~/.jdks
if(DEFINED ENV{JAVA_HOME} AND EXISTS "$ENV{JAVA_HOME}/include/jni.h")
    file(TO_CMAKE_PATH "$ENV{JAVA_HOME}" JDK_ROOT)
else()
    # Search all subdirectories inside ~/.jdks for jni.h
    file(GLOB_RECURSE FOUND_JNI "${USER_HOME}/.jdks/*/include/jni.h")
    if(FOUND_JNI)
        list(GET FOUND_JNI 0 FIRST_JNI)
        # Extract the parent JDK root directory from .../include/jni.h
        get_filename_component(JNI_INC_DIR "${FIRST_JNI}" DIRECTORY)
        get_filename_component(JDK_ROOT "${JNI_INC_DIR}" DIRECTORY)
    endif()
endif()

if(NOT JDK_ROOT OR NOT EXISTS "${JDK_ROOT}/include/jni.h")
    message(FATAL_ERROR "Could not find a valid JDK containing include/jni.h. Please set JAVA_HOME.")
endif()

message(STATUS "Auto-detected JDK Root: ${JDK_ROOT}")

add_library(gc_user SHARED
        src/agent_core.cpp
        src/agent_entry.cpp
)

target_include_directories(gc_user PRIVATE
        "${CMAKE_CURRENT_SOURCE_DIR}/include"
        "${JDK_ROOT}/include"
        "${JDK_ROOT}/include/win32"
)