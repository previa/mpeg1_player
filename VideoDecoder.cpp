#include "VideoDecoder.h"
#include <math.h>
#include <chrono>

#define STB_IMAGE_WRITE_IMPLEMENTATION
#include "stb_image_write.h"

#define PICTURE_START_CODE              0x00
#define SLICE_CODE_START                0x01
#define SLICE_CODE_END                  0xAF
#define USER_DATA_START_CODE            0xB2
#define SEQUENCE_HEADER_START_CODE      0xB3
#define SEQUENCE_ERROR_CODE             0xB4
#define EXTENSION_START_CODE            0xB5
#define SEQUENCE_END_CODE               0xB7
#define GROUP_START_CODE                0xB8

#define PICTURE_TYPE_I                  1
#define PICTURE_TYPE_P                  2
#define PICTURE_TYPE_B                  3   // Unsupported (for now)
#define PICTURE_TYPE_D                  4   // Unsupported

#define PI                              3.1415926

static const int sign(int n) {
    if(n > 0) {
        return 1;
    }

    if(n == 0) {
        return 0;
    }

    return -1;
}

VideoDecoder::VideoDecoder(BitStream *stream, queue<Mat *> *display_buffer) {
    this->stream = stream;
    this->display_buffer = display_buffer;
}

void VideoDecoder::decode() {
    video_sequence();
}

void VideoDecoder::video_sequence() {
    stream->next_start_code();
    do {
        sequence_header();

        do {
            group_of_pictures();
        } while(stream->start_code == GROUP_START_CODE);
    } while(stream->start_code == SEQUENCE_HEADER_START_CODE);
}

void VideoDecoder::sequence_header() {
    // The width of the displayable part of each luminance picture in pixels. (left-aligned)
    width = stream->consume(12);
    // The height of the displayable part of each luminance picture in pixels. (top-aligned)
    height = stream->consume(12);

    aspect_ratio = ASPECT_RATIO[stream->consume(4)];
    frame_rate = FRAME_RATE[stream->consume(4)];

    // The bit rate of the bit stream measured in units of 400 bits/second, rounded upwards
    // 0 is forbidden, 3FFFF is variable bit rate
    bit_rate = stream->consume(18);

    // Marker bit
    stream->skip(1);

    // vbv buffer size
    stream->skip(10);

    // constrained parameter flag
    stream->skip(1);

    bool load_intra_quantizer_matrix = stream->consume(1);
    if(load_intra_quantizer_matrix) {
        for(int i = 0; i < 8; i++) {
            for(int j = 0; j < 8; j++) {
                int index = ZIG_ZAG[i][j];
                intra_quantizer_matrix[index/8][index%8] = stream->consume(8);
            }
        }
    } else {
        memcpy(intra_quantizer_matrix, DEFAULT_INTRA_QUANTIZER_MATRIX, 64);
    }

    bool load_non_intra_quantizer_matrix = stream->consume(1);
    if(load_non_intra_quantizer_matrix) {
        for(int i = 0; i < 8; i++) {
            for(int j = 0; j < 8; j++) {
                int index = ZIG_ZAG[i][j];
                non_intra_quantizer_matrix[index/8][index%8] = stream->consume(8);
            }
        }
    } else {
        memcpy(non_intra_quantizer_matrix, DEFAULT_NON_INTRA_QUANTIZER_MATRIX, 64);
    }

    // The width of the encoded luminance picture in macroblocks
    mb_width = (width + 15) >> 4;

    // The height of the encoded luminance picture in macroblocks
    mb_height = (height + 15) >> 4;


    // Skip extension and user data
    while(stream->start_code != GROUP_START_CODE) {
        stream->next_start_code();
    }

    init_frames();

    printf("Width: %d\nHeight: %d\n", width, height);
    printf("mb_width: %d\nmb_height: %d\n", mb_width, mb_height);
    printf("Frame rate: %0.2f\n", frame_rate);
}

void VideoDecoder::init_frames() {
    frame_current = (Frame*)malloc(sizeof(Frame));
    frame_current->y = (uint8_t*)malloc(sizeof(uint8_t)*width*height);
    frame_current->cb = (uint8_t*)malloc(sizeof(uint8_t)*width*height);
    frame_current->cr = (uint8_t*)malloc(sizeof(uint8_t)*width*height);

    frame_prev = (Frame*)malloc(sizeof(Frame));
    frame_prev->y = (uint8_t*)malloc(sizeof(uint8_t)*width*height);
    frame_prev->cb = (uint8_t*)malloc(sizeof(uint8_t)*width*height);
    frame_prev->cr = (uint8_t*)malloc(sizeof(uint8_t)*width*height);
}

void VideoDecoder::set_prev_frame() {
    memcpy(frame_prev->y, frame_current->y, sizeof(uint8_t)*width*height);
    memcpy(frame_prev->cb, frame_current->cb, sizeof(uint8_t)*width*height);
    memcpy(frame_prev->cr, frame_current->cr, sizeof(uint8_t)*width*height);
}

void VideoDecoder::group_of_pictures() {
    // Time code
    stream->skip(25);

    // Closed GOP
    stream->skip(1);

    // Broken link
    stream->skip(1);

    // Skip extension and user data
    while(stream->start_code != PICTURE_START_CODE) {
        stream->next_start_code();
    }

    using std::chrono::high_resolution_clock;
    using std::chrono::duration_cast;
    using std::chrono::duration;
    using std::chrono::milliseconds;

    do {
        auto t1 = high_resolution_clock::now();
        picture();
        auto t2 = high_resolution_clock::now();

        auto ms_int = duration_cast<milliseconds>(t2 - t1);

        /* Getting number of milliseconds as a double. */
        duration<double, std::milli> ms_double = t2 - t1;

        printf("\t%0.5f ms\n", ms_double.count());

        add_frame_to_buffer();
        // write_image();

        while(stream->start_code != PICTURE_START_CODE) {
            stream->next_start_code();
        }

        set_prev_frame();
    } while(stream->start_code == PICTURE_START_CODE);
}

void VideoDecoder::picture() {
    temporal_reference = stream->consume(10);
    picture_coding_type = stream->consume(3);
    // printf("Picture: %d (%d)\n", temporal_reference, picture_coding_type);

    if(picture_coding_type == PICTURE_TYPE_B) { // Unsupported (for now)
        stream->next_start_code();
        return;
    }

    // vbv delay
    stream->skip(16);

    if(picture_coding_type == PICTURE_TYPE_P || picture_coding_type == PICTURE_TYPE_B) {
        full_pel_forward_vector = stream->consume(1);
        int forward_f_code = stream->consume(3);

        if(forward_f_code == 0) { // Forbidden value
            return;
        }

        // forward_r_size and forward_f are used in the process of decoding the forward 
        // motion vectors
        forward_r_size = forward_f_code - 1;
        forward_f = 1 << forward_r_size;
    }

    if(picture_coding_type == PICTURE_TYPE_B) {
        full_pel_backward_vector = stream->consume(1);
        int backward_f_code = stream->consume(3);

        if(backward_f_code == 0) { // Forbidden value
            return;
        }

        // backward_r_size and backward_f are used in the process of decoding the backward
        // motion vectors
        backward_r_size = backward_f_code - 1;
        backward_f = 1 << backward_r_size;
    }

    // Skip user and extension data + extra picture information
    do {
        stream->next_start_code();
    } while(!(stream->start_code >= SLICE_CODE_START &&
                stream->start_code <= SLICE_CODE_END));

    do {
        slice();
        if(macroblock_address == (mb_width * mb_height) - 1) {
            break;
        }
        stream->next_start_code();
    } while(stream->start_code >= SLICE_CODE_START && 
            stream->start_code <= SLICE_CODE_END);
}

void VideoDecoder::slice() {
    slice_vertical_position = stream->start_code & 0x000000FF;
    // printf("\tSlice:\t%d @ %lu\n", slice_vertical_position, stream->bit_index);

    quantizer_scale = stream->consume(5);

    macroblock_address = (slice_vertical_position - 1) * mb_width - 1;
    dct_dc_past[0] = dct_dc_past[1] = dct_dc_past[2] = 1024;
    past_intra_address = -2;
    recon_right_for_prev = recon_down_for_prev = 0;
    first_mb_in_slice = true;

    // Skip extra slice information
    while(stream->consume(1)) {
        stream->skip(8);
    }

    do {
        macroblock();
    } while(macroblock_address < (mb_width*mb_height) - 1 && 
                stream->no_start_code());
}

void VideoDecoder::macroblock() {
    int increment = 0;
    int t = read_vlc(stream, MACROBLOCK_ADDRESS_INCREMENT);

    while(t == 34) {
        t = read_vlc(stream, MACROBLOCK_ADDRESS_INCREMENT);
    }

    while(t == 35) {
        increment += 33;
        t = read_vlc(stream, MACROBLOCK_ADDRESS_INCREMENT);
    }

    increment += t;

    if(first_mb_in_slice) {
        first_mb_in_slice = false;
        macroblock_address += increment;
    } else {
        if(increment > 1) {
            recon_down_for_prev = recon_right_for_prev = 0;
            recon_down_for = recon_right_for = 0;
        }

        while(increment > 1) {
            macroblock_address++;
            predict_macroblock();
            increment--;
        }

        macroblock_address++;
    }

    mb_row = macroblock_address / mb_width;
    mb_col = macroblock_address % mb_width;

    if(mb_col >= mb_width || mb_row >= mb_height) {
        fputs("Wrong macroblock dimensions\n", stderr);
        exit(1);
    }

    if(picture_coding_type == PICTURE_TYPE_I) {
        mb_type = read_vlc(stream, MACROBLOCK_TYPE_I);
    } else if(picture_coding_type == PICTURE_TYPE_P) {
        mb_type = read_vlc(stream, MACROBLOCK_TYPE_P);
    }

    macroblock_intra = (mb_type & 0x01);
    macroblock_pattern  = (mb_type & 0x02);
    macroblock_motion_backward = (mb_type & 0x04);
    macroblock_motion_forward = (mb_type & 0x08);
    macroblock_quant = (mb_type & 0x10);

    if(macroblock_quant) {
        quantizer_scale = stream->consume(5);
    }

    if(macroblock_motion_forward) {
        motion_horizontal_forward_code = read_vlc(stream, MOTION_CODE);
        if((forward_f != 1) && (motion_horizontal_forward_code != 0)) {
            motion_horizontal_forward_r = stream->consume(forward_r_size);
        }

        motion_vertical_forward_code = read_vlc(stream, MOTION_CODE);
        if((forward_f != 1) && (motion_vertical_forward_code != 0)) {
            motion_vertical_forward_r = stream->consume(forward_r_size);
        }

        reconstruct_forward_motion_vectors();
    } else {
        recon_down_for = recon_right_for = 0;
    }

    // TODO: Implement macroblock_motion_backward for B-frames

    int cbp = (macroblock_pattern != 0) ? 
                read_vlc(stream, CODE_BLOCK_PATTERN) :
                (macroblock_intra ? 0x3F : 0);

    if(macroblock_intra) {
        recon_down_for = recon_right_for = 0;
        recon_down_for_prev = recon_right_for_prev = 0;
    } else {
        predict_macroblock();
    }

    for(int i = 0, mask = 0x20; i < 6; i++) {
        if((cbp & mask) != 0) {
            block(i);
        }
        mask >>= 1;
    }

    decode_blocks();
    add_macroblock_to_frame();
    reset_blocks();

    if(macroblock_intra) {
        past_intra_address = macroblock_address;
    }

    if(!macroblock_motion_forward) {
        recon_down_for_prev = recon_right_for_prev = 0;
    }
}

void VideoDecoder::predict_macroblock() {
    mb_row = macroblock_address / mb_width;
    mb_col = macroblock_address % mb_width;

    // Compute motion vectors for luminance
    int right_for = recon_right_for >> 1;
    int down_for = recon_down_for >> 1;

    int right_half_for = recon_right_for - 2 * right_for;
    int down_half_for = recon_down_for - 2 * down_for;

    // Compute motion vectors for chrominance
    int right_for_c = (recon_right_for/2) >> 1;
    int down_for_c = (recon_down_for/2) >> 1;

    int right_half_for_c = recon_right_for/2 - 2 * right_for_c;
    int down_half_for_c = recon_down_for/2 - 2 * down_for_c;

    for(int i = 0; i < 16; i++) {
        for(int j = 0; j < 16; j++) {
            int row = (mb_row * 16) + i + down_for;
            int col = (mb_col * 16) + j + right_for;
            int index = (row - down_for) * width + (col - right_for);

            copy_macroblock_into_frame(frame_current->y, frame_prev->y, index, row, col, right_half_for, down_half_for);

            row = mb_row * 16 + i + down_for_c;
            col = mb_col * 16 + j + right_for_c;
            index = (row - down_for_c) * width + (col - right_for_c);

            copy_macroblock_into_frame(frame_current->cb, frame_prev->cb, index, row, col, right_half_for_c, down_half_for_c);
            copy_macroblock_into_frame(frame_current->cr, frame_prev->cr, index, row, col, right_half_for_c, down_half_for_c);
        }
    }
}

void VideoDecoder::copy_macroblock_into_frame(uint8_t *dest, uint8_t *src, int index, int row, int col, int right, int down) {
    if(!right && !down) {
        dest[index] = src[row * width + col];   
    }

    if(!right && down) {
        dest[index] = (src[row * width + col] + src[(row + 1) * width + col]) / 2;
    }

    if(right && !down) {
        dest[index] = (src[row * width + col] + src[row * width + (col + 1)]) / 2;
    }

    if(right && down) {
        dest[index] = (src[row * width + col] + 
                       src[(row + 1) * width + col] + 
                       src[row * width + (col + 1)] +
                       src[(row + 1) * width + (col + 1)]) / 4;
    }
}

void VideoDecoder::add_macroblock_to_frame() {
    mb_row = macroblock_address / mb_width;
    mb_col = macroblock_address % mb_width;

    for(int i = 0; i < 8; i++) {
        for(int j = 0; j < 8; j++) {
            int row = mb_row * 16 + i;
            int col = mb_col * 16 + j;
            if(macroblock_intra)
                frame_current->y[row * width + col] = dct_recon[0][i*8 + j];
            else
                frame_current->y[row * width + col] += dct_recon[0][i*8 + j];

            row = mb_row * 16 + i;
            col = mb_col * 16 + j + 8;
            if(macroblock_intra)
                frame_current->y[row * width + col] = dct_recon[1][i*8 + j];
            else
                frame_current->y[row * width + col] += dct_recon[1][i*8 + j];

            row = mb_row * 16 + i + 8;
            col = mb_col * 16 + j;
            if(macroblock_intra)
                frame_current->y[row * width + col] = dct_recon[2][i*8 + j];
            else
                frame_current->y[row * width + col] += dct_recon[2][i*8 + j];

            row = mb_row * 16 + i + 8;
            col = mb_col * 16 + j + 8;
            if(macroblock_intra)
                frame_current->y[row * width + col] = dct_recon[3][i*8 + j];
            else
                frame_current->y[row * width + col] += dct_recon[3][i*8 + j];


            for(int k = 0; k < 2; k++) {
                for(int p = 0; p < 2; p++) {
                    row = mb_row * 16 + i * 2 + k;
                    col = mb_col * 16 + j * 2 + p;

                    if(macroblock_intra)
                        frame_current->cb[row * width + col] = dct_recon[4][i*8 + j];
                    else
                        frame_current->cb[row * width + col] += dct_recon[4][i*8 + j];


                    if(macroblock_intra)
                        frame_current->cr[row * width + col] = dct_recon[5][i*8 + j];
                    else
                        frame_current->cr[row * width + col] += dct_recon[5][i*8 + j];
                }
            }
        }
    }
}

void VideoDecoder::reconstruct_forward_motion_vectors() {
    int complement_horizontal_forward_r;
    int complement_vertical_forward_r;
    int right_little;
    int right_big;
    int down_little;
    int down_big;

    if(forward_f == 1 || motion_horizontal_forward_code == 0) {
        complement_horizontal_forward_r = 0;
    } else {
        complement_horizontal_forward_r = forward_f - 1 - motion_horizontal_forward_r;
    }

    if(forward_f == 1 || motion_vertical_forward_code == 0) {
        complement_vertical_forward_r = 0;
    } else {
        complement_vertical_forward_r = forward_f - 1 - motion_vertical_forward_r;
    }

    right_little = motion_horizontal_forward_code * forward_f;
    if(right_little == 0) {
        right_big = 0;
    } else {
        if(right_little > 0) {
            right_little = right_little - complement_horizontal_forward_r;
            right_big = right_little - 32 * forward_f;
        } else {
            right_little = right_little + complement_horizontal_forward_r;
            right_big = right_little + 32 * forward_f;
        }
    }

    down_little = motion_vertical_forward_code * forward_f;
    if(down_little == 0) {
        down_big = 0;
    } else {
        if(down_little > 0) {
            down_little = down_little - complement_vertical_forward_r;
            down_big = down_little - 32 * forward_f;
        } else {
            down_little = down_little + complement_vertical_forward_r;
            down_big = down_little + 32 * forward_f;
        }
    }

    int max = (16 * forward_f) - 1;
    int min = (-16 * forward_f);

    // Vector right
    int new_vector = recon_right_for_prev + right_little;
    if(new_vector <= max && new_vector >= min) {
        recon_right_for = recon_right_for_prev + right_little;
    } else {
        recon_right_for = recon_right_for_prev + right_big;
    }
    recon_right_for_prev = recon_right_for;

    // Vector down
    new_vector = recon_down_for_prev + down_little;
    if(new_vector <= max && new_vector >= min) {
        recon_down_for = recon_down_for_prev + down_little;
    } else {
        recon_down_for = recon_down_for_prev + down_big;
    }
    recon_down_for_prev = recon_down_for;

    if(full_pel_forward_vector) {
        recon_down_for = recon_down_for << 1;
    }
}

void VideoDecoder::block(int i) {
    int index = 0;
    if(macroblock_intra) {
        if(i < 4) { // Luminance block
            dct_dc_size_luminance = read_vlc(stream, DCT_SIZE_LUMINANCE);
            if(dct_dc_size_luminance != 0) {
                dct_dc_differential = stream->consume(dct_dc_size_luminance);

                if(dct_dc_differential & (1 << (dct_dc_size_luminance - 1))) {
                    dct_zz[i][0] = dct_dc_differential;
                } else {
                    dct_zz[i][0] = (-1 << (dct_dc_size_luminance)) | (dct_dc_differential + 1);
                }
            }
        } else {
            dct_dc_size_chrominance = read_vlc(stream, DCT_SIZE_CHROMINANCE);
            if(dct_dc_size_chrominance != 0) {
                dct_dc_differential = stream->consume(dct_dc_size_chrominance);

                if(dct_dc_differential & (1 << (dct_dc_size_chrominance - 1))) {
                    dct_zz[i][0] = dct_dc_differential;
                } else {
                    dct_zz[i][0] = (-1 << (dct_dc_size_chrominance)) | (dct_dc_differential + 1);
                }
            }
        }
    
        index = 1;
    }

    int level = 0;
    while(true) {
        int run = 0;
        uint16_t coeff = read_vlc_uint(stream, DCT_COEFF);

        if((coeff == 0x0001) & (index > 0) && (stream->consume(1) == 0)) {
            break;
        }

        if(coeff == 0xFFFF) { // Escape
            run = stream->consume(6);
            level = stream->consume(8);

            if(level == 0) {
                level = stream->consume(8);
            } else if(level == 128) {
                level = stream->consume(8) - 256;
            } else if(level > 128) {
                level = level - 256;
            }
        } else {
            run = coeff >> 8;
            level = coeff & 0xFF;

            if(stream->consume(1)) {
                level = -level;
            }
        }

        index += run;
        dct_zz[i][index] = level;
        index++;
    }
}

void VideoDecoder::print_block(int block) {
    for(int i = 0; i < 64; i++) {
        if(i%8 == 0) {
            printf("\n");
        }

        printf("%5d ", dct_zz[block][i]);
    }
    printf("\n");
}

void VideoDecoder::decode_blocks() {
    if(macroblock_intra) {
        decode_intra_blocks();
    } else {
        dequantize(false);
    }

    inverse_discrete_cosine_transform();
    clamp_blocks();
}

void VideoDecoder::clamp_blocks() {
    for(int i = 0; i < 64; i++) {
        for(int j = 0; j < 6; j++) {
            if(dct_recon[j][i] > 255) {
                dct_recon[j][i] = 255;
            } else if(dct_recon[j][i] < 0) {
                dct_recon[j][i] = 0;
            }
        }
    }
}

void VideoDecoder::inverse_discrete_cosine_transform() {
    static const float m0 = 2.0 * cos(1.0 / 16.0 * 2.0 * M_PI);
    static const float m1 = 2.0 * cos(2.0 / 16.0 * 2.0 * M_PI);
    static const float m3 = 2.0 * cos(2.0 / 16.0 * 2.0 * M_PI);
    static const float m5 = 2.0 * cos(3.0 / 16.0 * 2.0 * M_PI);
    static const float m2 = m0 - m5;
    static const float m4 = m0 + m5;
    static const float s0 = cos(0.0 / 16.0 * M_PI) / sqrt(8);
    static const float s1 = cos(1.0 / 16.0 * M_PI) / 2.0;
    static const float s2 = cos(2.0 / 16.0 * M_PI) / 2.0;
    static const float s3 = cos(3.0 / 16.0 * M_PI) / 2.0;
    static const float s4 = cos(4.0 / 16.0 * M_PI) / 2.0;
    static const float s5 = cos(5.0 / 16.0 * M_PI) / 2.0;
    static const float s6 = cos(6.0 / 16.0 * M_PI) / 2.0;
    static const float s7 = cos(7.0 / 16.0 * M_PI) / 2.0;


    for(int b = 0; b < 6; b++) {
        int *block_component = dct_recon[b];
        for (int k = 0; k < 8; ++k) {
            const float g0 = block_component[0 * 8 + k] * s0;
            const float g1 = block_component[4 * 8 + k] * s4;
            const float g2 = block_component[2 * 8 + k] * s2;
            const float g3 = block_component[6 * 8 + k] * s6;
            const float g4 = block_component[5 * 8 + k] * s5;
            const float g5 = block_component[1 * 8 + k] * s1;
            const float g6 = block_component[7 * 8 + k] * s7;
            const float g7 = block_component[3 * 8 + k] * s3;

            const float f0 = g0;
            const float f1 = g1;
            const float f2 = g2;
            const float f3 = g3;
            const float f4 = g4 - g7;
            const float f5 = g5 + g6;
            const float f6 = g5 - g6;
            const float f7 = g4 + g7;

            const float e0 = f0;
            const float e1 = f1;
            const float e2 = f2 - f3;
            const float e3 = f2 + f3;
            const float e4 = f4;
            const float e5 = f5 - f7;
            const float e6 = f6;
            const float e7 = f5 + f7;
            const float e8 = f4 + f6;

            const float d0 = e0;
            const float d1 = e1;
            const float d2 = e2 * m1;
            const float d3 = e3;
            const float d4 = e4 * m2;
            const float d5 = e5 * m3;
            const float d6 = e6 * m4;
            const float d7 = e7;
            const float d8 = e8 * m5;

            const float c0 = d0 + d1;
            const float c1 = d0 - d1;
            const float c2 = d2 - d3;
            const float c3 = d3;
            const float c4 = d4 + d8;
            const float c5 = d5 + d7;
            const float c6 = d6 - d8;
            const float c7 = d7;
            const float c8 = c5 - c6;

            const float b0 = c0 + c3;
            const float b1 = c1 + c2;
            const float b2 = c1 - c2;
            const float b3 = c0 - c3;
            const float b4 = c4 - c8;
            const float b5 = c8;
            const float b6 = c6 - c7;
            const float b7 = c7;

            block_component[0 * 8 + k] = b0 + b7;
            block_component[1 * 8 + k] = b1 + b6;
            block_component[2 * 8 + k] = b2 + b5;
            block_component[3 * 8 + k] = b3 + b4;
            block_component[4 * 8 + k] = b3 - b4;
            block_component[5 * 8 + k] = b2 - b5;
            block_component[6 * 8 + k] = b1 - b6;
            block_component[7 * 8 + k] = b0 - b7;
        }


        for (int l = 0; l < 8; ++l) {
            const float g0 = block_component[l * 8 + 0] * s0;
            const float g1 = block_component[l * 8 + 4] * s4;
            const float g2 = block_component[l * 8 + 2] * s2;
            const float g3 = block_component[l * 8 + 6] * s6;
            const float g4 = block_component[l * 8 + 5] * s5;
            const float g5 = block_component[l * 8 + 1] * s1;
            const float g6 = block_component[l * 8 + 7] * s7;
            const float g7 = block_component[l * 8 + 3] * s3;

            const float f0 = g0;
            const float f1 = g1;
            const float f2 = g2;
            const float f3 = g3;
            const float f4 = g4 - g7;
            const float f5 = g5 + g6;
            const float f6 = g5 - g6;
            const float f7 = g4 + g7;

            const float e0 = f0;
            const float e1 = f1;
            const float e2 = f2 - f3;
            const float e3 = f2 + f3;
            const float e4 = f4;
            const float e5 = f5 - f7;
            const float e6 = f6;
            const float e7 = f5 + f7;
            const float e8 = f4 + f6;

            const float d0 = e0;
            const float d1 = e1;
            const float d2 = e2 * m1;
            const float d3 = e3;
            const float d4 = e4 * m2;
            const float d5 = e5 * m3;
            const float d6 = e6 * m4;
            const float d7 = e7;
            const float d8 = e8 * m5;

            const float c0 = d0 + d1;
            const float c1 = d0 - d1;
            const float c2 = d2 - d3;
            const float c3 = d3;
            const float c4 = d4 + d8;
            const float c5 = d5 + d7;
            const float c6 = d6 - d8;
            const float c7 = d7;
            const float c8 = c5 - c6;

            const float b0 = c0 + c3;
            const float b1 = c1 + c2;
            const float b2 = c1 - c2;
            const float b3 = c0 - c3;
            const float b4 = c4 - c8;
            const float b5 = c8;
            const float b6 = c6 - c7;
            const float b7 = c7;

            block_component[l * 8 + 0] = b0 + b7;
            block_component[l * 8 + 1] = b1 + b6;
            block_component[l * 8 + 2] = b2 + b5;
            block_component[l * 8 + 3] = b3 + b4;
            block_component[l * 8 + 4] = b3 - b4;
            block_component[l * 8 + 5] = b2 - b5;
            block_component[l * 8 + 6] = b1 - b6;
            block_component[l * 8 + 7] = b0 - b7;
        }
    }


    // double idct, cb, sum, cr;

    // int output[8][8], gray_level;

    // for(int b = 0; b < 6; b++) {        
    //     for(int i = 0; i < 8; i++) {
    //         for(int j = 0; j < 8; j++) {
    //             sum = 0.0;

    //             for(int u = 0; u < 8; u++) {
    //                 for(int v = 0; v < 8; v++) {
    //                     if(u == 0) {
    //                         cb = 0.707106781;
    //                     } else {
    //                         cb = 1.0;
    //                     }

    //                     if(v == 0) {
    //                         cr = 0.707106781;
    //                     } else {
    //                         cr = 1.0;
    //                     }

    //                     gray_level = dct_recon[b][u * 8 + v];
    //                     idct = (gray_level * cb * cr * COS_DATA[i][u] * COS_DATA[j][v]);

    //                     sum += idct;
    //                 }
    //             }

    //             output[i][j] = (int)(0.25 * sum);
    //         }
    //     }

    //     memset(dct_recon[b], 0, sizeof(int)*64);
        
    //     for(int k = 0; k < 8; k++) {
    //         for(int l = 0; l < 8; l++) {
    //             dct_recon[b][k * 8 + l] = output[k][l];
    //         }
    //     }
    // }
}

void VideoDecoder::decode_intra_blocks() {
    dequantize(true);

    for(int i = 0; i < 6; i++) {
        int type = i < 4 ? 0 : (i == 4 ? 1 : 2);
        if(i == 0 || i == 4 || i == 5) {
            dct_recon[i][0] = dct_zz[i][0] * 8;
            if(macroblock_address - past_intra_address > 1) {
                dct_recon[i][0] = 128 * 8 + dct_recon[i][0];
            } else {
                dct_recon[i][0] = dct_dc_past[type] + dct_recon[i][0];
            }
        } else {
            dct_recon[i][0] = dct_dc_past[type] + dct_zz[i][0] * 8;
        }

        dct_dc_past[type] = dct_recon[i][0];
    } 
}

void VideoDecoder::dequantize(bool intra) {
    int value;
    for(int i = 0; i < 6; i++) {
    for(int m = 0; m < 8; m++) {
        for(int n = 0; n < 8; n++) {
            // for(int i = 0; i < 6; i++) {
                int index = ZIG_ZAG[m][n];
                if(intra) {
                    value = (2 * dct_zz[i][index] * quantizer_scale * intra_quantizer_matrix[m][n]);
                } else {
                    value = (((2 * dct_zz[i][index]) + sign(dct_zz[i][index])) * quantizer_scale * non_intra_quantizer_matrix[m][n]);
                }
                dct_recon[i][m * 8 + n] = value >> 4;

                if((dct_recon[i][m * 8 + n] & 1) == 0) {
                    dct_recon[i][m * 8 + n] = dct_recon[i][m * 8 + n] - sign(dct_recon[i][m * 8 + n]);
                }

                if(dct_recon[i][m * 8 + n] > 2047) {
                    dct_recon[i][m * 8 + n] = 2047;
                }

                if(dct_recon[i][m * 8 + n] < -2048) {
                    dct_recon[i][m * 8 + n] = -2048;
                }

                if(!intra) {
                    if(dct_zz[i][index] == 0) {
                        dct_recon[i][m * 8 + n] = 0;
                    }
                }
            }
        }
    }    
}

void VideoDecoder::reset_blocks() {
    memset(dct_zz, 0, sizeof(int)*6*64);
    memset(dct_recon, 0, sizeof(int)*6*64);
}

void VideoDecoder::frame_to_rgb(uint8_t *buffer) {
    double r,g,b,y,cb,cr;
    int index = 0;

    for(int i = 0; i < height; i++) {
        for(int j = 0; j < width; j++) {
            y = (double)frame_current->y[i * width + j];
            cb = (double)frame_current->cb[i * width + j];
            cr = (double)frame_current->cr[i * width + j];

            r = y + 1.402 * (cr - 128.0);
            g = y - 0.34414 * (cb - 128.0) - 0.71414 * (cr - 128.0);
            b = y + 1.774 * (cb - 128.0);

            r = r > 255 ? 255 : r;
            g = g > 255 ? 255 : g;
            b = g > 255 ? 255 : b;

            r = r < 0 ? 0 : r;
            g = g < 0 ? 0 : g;
            b = b < 0 ? 0 : b;

            buffer[index++] = r;
            buffer[index++] = g;
            buffer[index++] = b;
        }
    }
}

void VideoDecoder::frame_to_rgb(Mat *result) {
    double r,g,b,y,cb,cr;
    int index = 0;

    for(int i = 0; i < height; i++) {
        for(int j = 0; j < width; j++) {
            y = (double)frame_current->y[i * width + j];
            cb = (double)frame_current->cb[i * width + j];
            cr = (double)frame_current->cr[i * width + j];

            r = y + 1.402 * (cr - 128.0);
            g = y - 0.34414 * (cb - 128.0) - 0.71414 * (cr - 128.0);
            b = y + 1.774 * (cb - 128.0);

            r = r > 255 ? 255 : r;
            g = g > 255 ? 255 : g;
            b = g > 255 ? 255 : b;

            r = r < 0 ? 0 : r;
            g = g < 0 ? 0 : g;
            b = b < 0 ? 0 : b;

            result->at<Vec3b>(i,j)[0] = b;
            result->at<Vec3b>(i,j)[1] = g;
            result->at<Vec3b>(i,j)[2] = r;
        }
    }
}

void VideoDecoder::write_image() {
    uint8_t* rgb_buffer = (uint8_t*)malloc(sizeof(uint8_t)*height*width*3);
    char png_name[16];
    
    frame_to_rgb(rgb_buffer);
    current_picture_nr++;

    sprintf(png_name, "./images/%06lu.png", current_picture_nr);
    printf("Writing %s\n", png_name);
    stbi_write_png(png_name, width, height, 3, rgb_buffer, width*3);  
}

void VideoDecoder::add_frame_to_buffer() {
    Mat *buffer = new Mat(height, width, CV_8UC3);

    frame_to_rgb(buffer);
    display_buffer->push(buffer);
}