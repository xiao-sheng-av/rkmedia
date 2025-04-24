#最小支持的cmake版本
cmake_minimum_required(VERSION 3.10)  

#设置项目名称和编译器
project(test VERSION 0.0.1 LANGUAGES CXX)

#支持的c++版本
set(CMAKE_CXX_STANDARD 11)   
#强制要求编译器使用该标准，如果编译器不支持 C++11，会导致构建失败
set(CMAKE_CXX_STANDARD_REQUIRED True) 


set(CMAKE_CXX_COMPILER "/opt/rv1126_rv1109_linux/prebuilts/gcc/linux-x86/arm/gcc-arm-8.3-2019.03-x86_64-arm-linux-gnueabihf/bin/arm-linux-gnueabihf-g++")

#PROJECT_SOURCE_DIR为CMakeList.txt的目录
include_directories(${PROJECT_SOURCE_DIR}/include/rkmedia)
include_directories(${PROJECT_SOURCE_DIR}/include/easymedia)
include_directories(${PROJECT_SOURCE_DIR}/include/ffmpeg)
include_directories(${PROJECT_SOURCE_DIR}/include/ffmpeg/libavformat)
include_directories(${PROJECT_SOURCE_DIR}/include/ffmpeg/libavcodec)
include_directories(${PROJECT_SOURCE_DIR}/include/ffmpeg/libavdevice)
include_directories(${PROJECT_SOURCE_DIR}/include/ffmpeg/libavfilter)
include_directories(${PROJECT_SOURCE_DIR}/include/ffmpeg/libavutil)
include_directories(${PROJECT_SOURCE_DIR}/include/ffmpeg/libswscale)
include_directories(${PROJECT_SOURCE_DIR}/include/ffmpeg/libswresample)

link_directories(${CMAKE_SOURCE_DIR}/lib)

#指定要生成的可执行文件和其源文件
add_executable(rv1126_ffmpeg_main rv1126_ffmpeg_main.cpp ffmpeg_rtsp.cpp rkmedia.cpp )

target_link_libraries(rv1126_ffmpeg_main
${PROJECT_SOURCE_DIR}/lib/libeasymedia.so
${PROJECT_SOURCE_DIR}/lib/libdrm.so
${PROJECT_SOURCE_DIR}/lib/libasound.so
${PROJECT_SOURCE_DIR}/lib/libBasicUsageEnvironment.so
${PROJECT_SOURCE_DIR}/lib/libUsageEnvironment.so
${PROJECT_SOURCE_DIR}/lib/libv4l2.so
${PROJECT_SOURCE_DIR}/lib/librga.so
${PROJECT_SOURCE_DIR}/lib/librknn_api.so
${PROJECT_SOURCE_DIR}/lib/librockface.so
${PROJECT_SOURCE_DIR}/lib/libmd_share.so
${PROJECT_SOURCE_DIR}/lib/libod_share.so
${PROJECT_SOURCE_DIR}/lib/librockchip_mpp.so.1
${PROJECT_SOURCE_DIR}/lib/libliveMedia.so.61
${PROJECT_SOURCE_DIR}/lib/libgroupsock.so.8
${PROJECT_SOURCE_DIR}/lib/libRKAP_3A.so
${PROJECT_SOURCE_DIR}/lib/libRKAP_ANR.so
${PROJECT_SOURCE_DIR}/lib/libsqlite3.so.0
${PROJECT_SOURCE_DIR}/lib/libRKAP_Common.so
${PROJECT_SOURCE_DIR}/lib/librkaiq.so
${PROJECT_SOURCE_DIR}/lib/libv4lconvert.so.0
${PROJECT_SOURCE_DIR}/lib/libpthread-2.28.so
${PROJECT_SOURCE_DIR}/lib/libavformat.so
${PROJECT_SOURCE_DIR}/lib/libavcodec.so
${PROJECT_SOURCE_DIR}/lib/libavutil.so 
${PROJECT_SOURCE_DIR}/lib/libswresample.so
${PROJECT_SOURCE_DIR}/lib/libswscale.so
${PROJECT_SOURCE_DIR}/lib/libx264.so.165
)
