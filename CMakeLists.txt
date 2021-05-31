project(mpeg1_player)

set(CMAKE_CXX_STANDARD 17)

add_executable(mpeg1_player main.cpp BitStream.cpp BitStream.h
                                    Demuxer.cpp Demuxer.h
                                    VideoDecoder.cpp VideoDecoder.h
                                    VLC.h)