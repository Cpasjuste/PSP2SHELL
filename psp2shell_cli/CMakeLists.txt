cmake_minimum_required(VERSION 3.0)
add_definitions(-std=c99)

set(CMAKE_SYSTEM_NAME "Linux")

project(psp2shell_cli)

add_executable(${PROJECT_NAME}
        source/main.c
        source/cmd.c
        source/utility.c
        ../common/p2s_cmd.c
        ../common/p2s_msg.c
        )

target_include_directories(${PROJECT_NAME} PRIVATE
        include
        ../common
        )

target_link_libraries(${PROJECT_NAME}
        readline
        termcap
        pthread
        )

install(TARGETS ${PROJECT_NAME} RUNTIME DESTINATION bin LIBRARY DESTINATION lib ARCHIVE DESTINATION lib)
