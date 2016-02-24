project(webserver)
cmake_minimum_required(VERSION 2.8)

list(APPEND CMAKE_MODULE_PATH "${CMAKE_CURRENT_LIST_DIR}/cmake")

aux_source_directory(. SRC_LIST)

# Add HTTP Parser
list(APPEND SRC_LIST http-parser/http_parser.c)

set(CMAKE_CXX_FLAGS "${CMAKE_CXX_FLAGS} -std=c++11 -Wall -Wextra")

# See: http://www.cmake.org/cmake/help/v3.0/module/FindThreads.html
find_package(Threads REQUIRED)
find_package(Libev REQUIRED)
#find_package(Magic REQUIRED)

include_directories(${MAGIC_INCLUDE_DIR})

set(EXE_NAME final)

add_executable(${EXE_NAME} ${SRC_LIST})
target_link_libraries(${EXE_NAME}
    ${CMAKE_THREAD_LIBS_INIT}
    ${LIBEV_LIBRARY}
    ${MAGIC_LIBRARY}
)

#get_property(WEBSERVER_NAME TARGET ${PROJECT_NAME} PROPERTY LOCATION)
#configure_file(${CMAKE_SOURCE_DIR}/final.in ./final @ONLY)
