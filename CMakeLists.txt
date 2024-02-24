cmake_minimum_required(VERSION 3.28)
project(tinyhttp)

set(CMAKE_CXX_STANDARD 23)
include_directories(include)
add_compile_options(-fconcepts-diagnostics-depth=2)
add_compile_definitions(-DENABLE_ANSI_DISPLAY)
add_executable(tinyhttp_reactor
        include/memory.h
        include/evchannel.h
        include/timer.h
        include/log.h
        include/stacktrace.h
        src/reactor.cpp
        include/io.h
        src/worker.cpp
)
target_link_options(tinyhttp_reactor PRIVATE
        -rdynamic
        -pthread
        -ldl
)
