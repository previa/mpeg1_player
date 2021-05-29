#include "BitStream.h"

typedef struct {
    int type {0};
    int length {0};
    double pts {0.0};
} MPEG1_Packet;

class Demuxer {
public:
    Demuxer(const char*);
    ~Demuxer();

    MPEG1_Packet get_packet(int);
    void add_packet(BitStream*, MPEG1_Packet);

    BitStream *file_stream {nullptr};
    BitStream *video_stream {nullptr};
    BitStream *audio_stream {nullptr};

private:
    FILE *fp;

    MPEG1_Packet get_video_packet();
    MPEG1_Packet get_audio_packet();

    double decode_time(BitStream*);

    double last_decoded_pts {0};
};