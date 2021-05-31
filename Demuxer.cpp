#include "Demuxer.h"

#define DEFAULT_READ_SIZE                   1024*32

#define MPEG1_VIDEO_PACKET_START_CODE       0xE0
#define MPEG1_AUDIO_PACKET_START_CODE       0xC0
// TODO: Support multiple audio streams

#define MPEG1_PACKET_TYPE_VIDEO             1
#define MPEG1_PACKET_TYPE_AUDIO             2


void load_data_from_file(BitStream *self, void *data) {
    if(!self->fp) {
        fputs("No file pointer was given!", stderr);
        exit(1);
    }

    if(self->data) { 
        self->data = (uint8_t*)realloc(self->data, DEFAULT_READ_SIZE + self->size);
    } else { // First packet
        self->data = (uint8_t*)malloc(DEFAULT_READ_SIZE * sizeof(uint8_t));
        self->size = 0;
    }

    size_t read = fread(self->data + self->size, sizeof(uint8_t), 
                        DEFAULT_READ_SIZE, self->fp);

    // printf("Read %lu\n", read);
    self->total_read += read;
    self->size += read;

    if(read == 0) {
        self->has_ended = true;
    }
}

void load_packet_from_parent(BitStream *self, void *data) {
    auto demuxer = (Demuxer*)data;
    BitStream *parent = demuxer->file_stream;

    // Find the packet
    do {
        parent->next_start_code();
    } while(parent->start_code != MPEG1_VIDEO_PACKET_START_CODE &&
            parent->start_code != MPEG1_AUDIO_PACKET_START_CODE && 
            parent->start_code != -1);

    if(parent->start_code == -1) {
        self->has_ended = true;
        return;
    }

    MPEG1_Packet packet = demuxer->get_packet(parent->start_code);
    demuxer->add_packet(self, packet);

    // Check if the correct packet was read 
    // if not read another packet
    if(self->type != packet.type) {
        load_packet_from_parent(self, data);
    }
}

void Demuxer::add_packet(BitStream *self, MPEG1_Packet packet) {
    if(!self->data) {
        self->data = (uint8_t*)malloc(packet.length * sizeof(uint8_t));
        self->size = 0;
    } else {
        self->data = (uint8_t*)realloc(self->data, self->size + packet.length);
    }

    size_t parent_byte_pos = file_stream->bit_index >> 3;
    memcpy(self->data + self->size, file_stream->data + parent_byte_pos, packet.length);

    self->total_read += packet.length;
    self->size += packet.length;

    file_stream->skip(packet.length);
}

Demuxer::Demuxer(const char *file) {
    fp = fopen(file, "rb");

    file_stream = new BitStream(fp);
    file_stream->load_callback = load_data_from_file;

    // Demux into (audio and) video stream
    video_stream = new BitStream(file_stream);
    video_stream->load_callback = load_packet_from_parent;
    video_stream->load_callback_data = this;
    video_stream->type = MPEG1_PACKET_TYPE_VIDEO;

    video_decoder = new VideoDecoder(video_stream);
    video_decoder->decode();

}

Demuxer::~Demuxer() {
    fclose(fp);

    if(file_stream->data) {
        free(file_stream->data);
    }

    if(video_stream->data) {
        free(video_stream->data);
    }

    delete file_stream;
    delete video_stream;
}

MPEG1_Packet Demuxer::get_packet(int start_code) {
    if(start_code == MPEG1_VIDEO_PACKET_START_CODE) {
        return get_video_packet();
    } else if(start_code == MPEG1_AUDIO_PACKET_START_CODE) {
        return get_audio_packet();
    } else {
        fputs("Unknown packet given!", stderr);
        exit(1);
    }
}

MPEG1_Packet Demuxer::get_video_packet() {
    MPEG1_Packet packet;
    packet.type = MPEG1_PACKET_TYPE_VIDEO;

    if(!file_stream->has_remaining(16)) {
        fputs("Unable to read packet length", stderr);
        exit(1);
    }

    packet.length = file_stream->consume(16);

    if(!file_stream->has_remaining(packet.length)) {
        fputs("Packet is corrupt", stderr);
        exit(1);
    }

    packet.length -= file_stream->skip_bytes_while(0xFF);

    // Skip P-STD
    if(file_stream->consume(2) == 0x01) {
        file_stream->skip_bytes_while(16);
        packet.length -= 2;
    }

    int pts_dts_marker = file_stream->consume(2);
    if(pts_dts_marker == 0x03) {
        packet.pts = decode_time(file_stream);
        last_decoded_pts = packet.pts;

        // Skip DTS
        file_stream->skip(40);

        packet.length -= 10;
    } else if(pts_dts_marker == 0x02) {
        packet.pts = decode_time(file_stream);
        last_decoded_pts = packet.pts;

        packet.length -= 5;
    } else if(pts_dts_marker == 0x00) {
        packet.pts = -1;

        file_stream->skip(4);

        packet.length -= 1;
    } else {
        fputs("Invalid PTS/DTS marker", stderr);
        exit(1);
    }

    return packet;
}

MPEG1_Packet Demuxer::get_audio_packet() {
    MPEG1_Packet packet;
    packet.type = MPEG1_PACKET_TYPE_AUDIO;

    return packet;
}

double Demuxer::decode_time(BitStream *stream) {
    int64_t clock = stream->consume(3) << 30;
    // Marker bit
    stream->skip(1);
    clock |= stream->consume(15) << 15;
    // Marker bit
    stream->skip(1);
    clock |= stream->consume(15);
    // Marker bit
    stream->skip(1);

    return (double)clock/90000.0;
}