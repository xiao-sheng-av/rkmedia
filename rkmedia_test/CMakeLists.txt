#最小支持的cmake版本
cmake_minimum_required(VERSION 3.10)  

#设置项目名称和编译器
project(test VERSION 0.0.1 LANGUAGES CXX)

#支持的c++版本
set(CMAKE_CXX_STANDARD 11)   

#强制要求编译器使用该标准，如果编译器不支持 C++11，会导致构建失败
set(CMAKE_CXX_STANDARD_REQUIRED True) 

#PROJECT_SOURCE_DIR为CMakeList.txt的目录
include_directories(${PROJECT_SOURCE_DIR}/include/rkmedia)
include_directories(${PROJECT_SOURCE_DIR}/include/easymedia)

link_directories(${CMAKE_SOURCE_DIR}/lib)

#指定要生成的可执行文件和其源文件
add_executable(first vi_get_frame_test.cpp )

target_link_libraries(first easymedia)  # 这里确保是实际的 Rockchip 库名称