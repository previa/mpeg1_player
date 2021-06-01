cmake_minimum_required(VERSION 3.16)

project(mpeg1_player)

set(CMAKE_CXX_STANDARD 17)

set(CMAKE_CXX_FLAGS_RELEASE "-O3")
set(THREADS_PREFER_PTHREAD_FLAG ON)

find_package(Threads REQUIRED)
find_package(OpenCV REQUIRED )

include_directories( ${OpenCV_INCLUDE_DIRS} )

add_executable(mpeg1_player main.cpp BitStream.cpp BitStream.h
                                    Demuxer.cpp Demuxer.h
                                    VideoDecoder.cpp VideoDecoder.h
                                    VLC.h)

target_link_libraries(mpeg1_player ${OpenCV_LIBS} Threads::Threads)
