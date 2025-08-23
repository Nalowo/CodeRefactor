set(CMAKE_BUILD_TYPE Debug)
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)

find_package(LLVM REQUIRED CONFIG)
find_package(Clang REQUIRED CONFIG)

add_library(refactor_tool_lib
    RefactorTool.cpp
)

target_include_directories(refactor_tool_lib
    PUBLIC
        ${CMAKE_SOURCE_DIR}/include
)

target_link_libraries(refactor_tool_lib
    PUBLIC
        clangTooling
        clangBasic
        clangASTMatchers
        clangRewrite
        clangFrontend
)

add_executable(refactor_tool main.cpp)

target_include_directories(
  refactor_tool
  PRIVATE
  "${CMAKE_CURRENT_SOURCE_DIR}/../include"
)
target_link_libraries(refactor_tool PRIVATE refactor_tool_lib)