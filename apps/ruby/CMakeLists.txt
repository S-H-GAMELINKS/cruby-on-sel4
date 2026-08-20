cmake_minimum_required(VERSION 3.16.0)

project(ruby C)

set(CAmkESCPP
    ON
    CACHE BOOL "" FORCE
)
includeGlobalComponents()

add_subdirectory(${CMAKE_CURRENT_LIST_DIR}/components/Ruby)

DeclareCAmkESRootserver(ruby.camkes)
add_simulate_test([=[wait_for "Hello World"]=])
