cmake_minimum_required(VERSION 3.1.3)

project(kvstore)

file(GLOB_RECURSE SRC src/* metadata.json)

find_package(Qt5 5.5.0 REQUIRED COMPONENTS Widgets Sql)

add_library(${PROJECT_NAME} SHARED ${SRC} ${PROJECT_NAME}.qrc)

target_include_directories(${PROJECT_NAME} PRIVATE src/)

target_link_libraries(${PROJECT_NAME}
    PRIVATE
        ${Qt5Widgets_LIBRARIES}
        ${Qt5Sql_LIBRARIES}
        albertcore
)

install(TARGETS ${PROJECT_NAME} LIBRARY DESTINATION ${CMAKE_INSTALL_LIBDIR}/albert/plugins)
