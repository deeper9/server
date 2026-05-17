cmake_minimum_required(VERSION 3.6)
project(sylar)

set(CMAKE_MODULE_PATH ${CMAKE_MODULE_PATH} ${CMAKE_CURRENT_SOURCE_DIR}/cmake)
include (cmake/utils.cmake)

set(CMAKE_VERBOSE_MAKEFILE ON)
set(CMAKE_CXX_FLAGS "$ENV{CXXFLAGS} -rdynamic -O3 -fPIC -ggdb -std=c++11 -Wall -Wno-deprecated -Werror -Wno-unused-function -Wno-builtin-macro-redefined -Wno-deprecated-declarations")
set(CMAKE_C_FLAGS "$ENV{CXXFLAGS} -rdynamic -O3 -fPIC -ggdb -std=c11 -Wall -Wno-deprecated -Werror -Wno-unused-function -Wno-builtin-macro-redefined -Wno-deprecated-declarations")

include_directories(.)

find_package(Boost REQUIRED)
if(Boost_FOUND)
    include_directories(${Boost_INCLUDE_DIRS})
endif()

set(LIB_SRC
    sylar/config.cpp
    sylar/env.cpp
    sylar/log.cpp
    sylar/mutex.cpp
    sylar/thread.cpp
    sylar/util.cpp
    sylar/fd_manager.cpp
    sylar/fiber.cpp
    sylar/hook.cpp
    sylar/iomanager.cpp
    sylar/scheduler.cpp
    sylar/timer.cpp
    )

add_library(sylar SHARED ${LIB_SRC})
force_redefine_file_macro_for_sources(sylar)

set(LIBS
    sylar
    pthread
    yaml-cpp
)
option(BUILD_TEST "ON for complile test" ON)
if(BUILD_TEST)
sylar_add_executable(test_log "tests/test_log.cpp" sylar "${LIBS}")
sylar_add_executable(test_config "tests/test_config.cpp" sylar "${LIBS}")
sylar_add_executable(test_thread "tests/test_thread.cpp" sylar "${LIBS}")
sylar_add_executable(test_fiber "tests/test_fiber.cpp" sylar "${LIBS}")
sylar_add_executable(test_scheduler "tests/test_scheduler.cpp" sylar "${LIBS}")
endif()

SET(EXECUTABLE_OUTPUT_PATH ${PROJECT_SOURCE_DIR}/bin)
SET(LIBRARY_OUTPUT_PATH ${PROJECT_SOURCE_DIR}/lib)
