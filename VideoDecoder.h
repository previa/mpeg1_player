#include "VLC.h"
#include <queue>
#include <opencv2/opencv.hpp>

using namespace std;
using namespace cv;

static const double ASPECT_RATIO[] = {
    0,
    1.0000,  // VGA etc. 
    0.6735,
    0.7031, // 16:9, 625line 
    0.7615,
    0.8055,
    0.8437, // 16:9, 525line 
    0.8935, 
    0.9375, // CCIR601, 625line 
    0.9815, 
    1.0255, 
    1.0695, 
    1.1250, // CCIR601, 525line 
    1.1575, 
    1.2015 
};

static const double FRAME_RATE[] = {
    0,
    23.976, 
    24, 
    25, 
    29.97, 
    30,
    50,
    59.94, 
    60,
};

static const int ZIG_ZAG[8][8] = {
    {0,   1,  5,  6, 14, 15, 27, 28}, 
    {2,   4,  7, 13, 16, 26, 29, 42}, 
    {3,   8, 12, 17, 25, 30, 41, 43}, 
    {9,  11, 18, 24, 31, 40, 44, 53}, 
    {10, 19, 23, 32, 39, 45, 52, 54}, 
    {20, 22, 33, 38, 46, 51, 55, 60},
    {21, 34, 37, 47, 50, 56, 59, 61}, 
    {35, 36, 48, 49, 57, 58, 62, 63}
};

static const uint8_t DEFAULT_INTRA_QUANTIZER_MATRIX[8][8] = {
    {8, 16, 19, 22, 26, 27, 29, 34},
    {16, 16, 22, 24, 27, 29, 34, 37}, 
    {19, 22, 26, 27, 29, 34, 34, 38},
    {22, 22, 26, 27, 29, 34, 37, 40},
    {22, 26, 27, 29, 32, 35, 40, 48},
    {26, 27, 29, 32, 35, 40, 48, 58},
    {26, 27, 29, 34, 38, 46, 56, 69}, 
    {27, 29, 35, 38, 46, 56, 69, 83}
};

static const uint8_t DEFAULT_NON_INTRA_QUANTIZER_MATRIX[] = {
    16, 16, 16, 16, 16, 16, 16, 16,
    16, 16, 16, 16, 16, 16, 16, 16, 
    16, 16, 16, 16, 16, 16, 16, 16, 
    16, 16, 16, 16, 16, 16, 16, 16, 
    16, 16, 16, 16, 16, 16, 16, 16, 
    16, 16, 16, 16, 16, 16, 16, 16,
    16, 16, 16, 16, 16, 16, 16, 16, 
    16, 16, 16, 16, 16, 16, 16, 16
};

static const VLC CODE_BLOCK_PATTERN[] = {
    {  1 << 1,    0}, {  2 << 1,    0},  //   0: x
    {  3 << 1,    0}, {  4 << 1,    0},  //   1: 0x
    {  5 << 1,    0}, {  6 << 1,    0},  //   2: 1x
    {  7 << 1,    0}, {  8 << 1,    0},  //   3: 00x
    {  9 << 1,    0}, { 10 << 1,    0},  //   4: 01x
    { 11 << 1,    0}, { 12 << 1,    0},  //   5: 10x
    { 13 << 1,    0}, {       0,   60},  //   6: 11x
    { 14 << 1,    0}, { 15 << 1,    0},  //   7: 000x
    { 16 << 1,    0}, { 17 << 1,    0},  //   8: 001x
    { 18 << 1,    0}, { 19 << 1,    0},  //   9: 010x
    { 20 << 1,    0}, { 21 << 1,    0},  //  10: 011x
    { 22 << 1,    0}, { 23 << 1,    0},  //  11: 100x
    {       0,   32}, {       0,   16},  //  12: 101x
    {       0,    8}, {       0,    4},  //  13: 110x
    { 24 << 1,    0}, { 25 << 1,    0},  //  14: 0000x
    { 26 << 1,    0}, { 27 << 1,    0},  //  15: 0001x
    { 28 << 1,    0}, { 29 << 1,    0},  //  16: 0010x
    { 30 << 1,    0}, { 31 << 1,    0},  //  17: 0011x
    {       0,   62}, {       0,    2},  //  18: 0100x
    {       0,   61}, {       0,    1},  //  19: 0101x
    {       0,   56}, {       0,   52},  //  20: 0110x
    {       0,   44}, {       0,   28},  //  21: 0111x
    {       0,   40}, {       0,   20},  //  22: 1000x
    {       0,   48}, {       0,   12},  //  23: 1001x
    { 32 << 1,    0}, { 33 << 1,    0},  //  24: 0000 0x
    { 34 << 1,    0}, { 35 << 1,    0},  //  25: 0000 1x
    { 36 << 1,    0}, { 37 << 1,    0},  //  26: 0001 0x
    { 38 << 1,    0}, { 39 << 1,    0},  //  27: 0001 1x
    { 40 << 1,    0}, { 41 << 1,    0},  //  28: 0010 0x
    { 42 << 1,    0}, { 43 << 1,    0},  //  29: 0010 1x
    {       0,   63}, {       0,    3},  //  30: 0011 0x
    {       0,   36}, {       0,   24},  //  31: 0011 1x
    { 44 << 1,    0}, { 45 << 1,    0},  //  32: 0000 00x
    { 46 << 1,    0}, { 47 << 1,    0},  //  33: 0000 01x
    { 48 << 1,    0}, { 49 << 1,    0},  //  34: 0000 10x
    { 50 << 1,    0}, { 51 << 1,    0},  //  35: 0000 11x
    { 52 << 1,    0}, { 53 << 1,    0},  //  36: 0001 00x
    { 54 << 1,    0}, { 55 << 1,    0},  //  37: 0001 01x
    { 56 << 1,    0}, { 57 << 1,    0},  //  38: 0001 10x
    { 58 << 1,    0}, { 59 << 1,    0},  //  39: 0001 11x
    {       0,   34}, {       0,   18},  //  40: 0010 00x
    {       0,   10}, {       0,    6},  //  41: 0010 01x
    {       0,   33}, {       0,   17},  //  42: 0010 10x
    {       0,    9}, {       0,    5},  //  43: 0010 11x
    {      -1,    0}, { 60 << 1,    0},  //  44: 0000 000x
    { 61 << 1,    0}, { 62 << 1,    0},  //  45: 0000 001x
    {       0,   58}, {       0,   54},  //  46: 0000 010x
    {       0,   46}, {       0,   30},  //  47: 0000 011x
    {       0,   57}, {       0,   53},  //  48: 0000 100x
    {       0,   45}, {       0,   29},  //  49: 0000 101x
    {       0,   38}, {       0,   26},  //  50: 0000 110x
    {       0,   37}, {       0,   25},  //  51: 0000 111x
    {       0,   43}, {       0,   23},  //  52: 0001 000x
    {       0,   51}, {       0,   15},  //  53: 0001 001x
    {       0,   42}, {       0,   22},  //  54: 0001 010x
    {       0,   50}, {       0,   14},  //  55: 0001 011x
    {       0,   41}, {       0,   21},  //  56: 0001 100x
    {       0,   49}, {       0,   13},  //  57: 0001 101x
    {       0,   35}, {       0,   19},  //  58: 0001 110x
    {       0,   11}, {       0,    7},  //  59: 0001 111x
    {       0,   39}, {       0,   27},  //  60: 0000 0001x
    {       0,   59}, {       0,   55},  //  61: 0000 0010x
    {       0,   47}, {       0,   31},  //  62: 0000 0011x
};

static const double COS_DATA[8][8] = {
    1.000000,0.980785,0.923880,0.831470,0.707107,0.555570,0.382683,0.195090,
    1.000000,0.831470,0.382683,-0.195090,-0.707107,-0.980785,-0.923880,-0.555570,
    1.000000,0.555570,-0.382683,-0.980785,-0.707107,0.195090,0.923880,0.831470,
    1.000000,0.195090,-0.923880,-0.555570,0.707107,0.831470,-0.382683,-0.980785,
    1.000000,-0.195090,-0.923880,0.555570,0.707107,-0.831470,-0.382684,0.980785,
    1.000000,-0.555570,-0.382683,0.980785,-0.707107,-0.195090,0.923880,-0.831470,
    1.000000,-0.831470,0.382683,0.195090,-0.707107,0.980785,-0.923879,0.555570,
    1.000000,-0.980785,0.923880,-0.831470,0.707107,-0.555570,0.382683,-0.195090
};

typedef struct {
    uint8_t *y;
    uint8_t *cb;
    uint8_t *cr;
} Frame;

class VideoDecoder {
public:
    VideoDecoder(BitStream*, queue<Mat *>*);

    void decode();

private:
    void video_sequence();
    void sequence_header();
    void group_of_pictures();
    void picture();
    void slice();
    void macroblock();
    void block(int);

    void predict_macroblock();
    void add_macroblock_to_frame();
    void copy_macroblock_into_frame(uint8_t*, uint8_t*, int, int, int, int, int);
    void reconstruct_forward_motion_vectors();

    void reset_blocks();
    void decode_blocks();
    void print_block(int);
    void decode_intra_blocks();
    void clamp_blocks();
    void inverse_discrete_cosine_transform();
    void dequantize(bool);

    void init_frames();
    void set_prev_frame();
    void frame_to_rgb(uint8_t*);
    void frame_to_rgb(Mat *result); 
    void add_frame_to_buffer();

    void write_image();

    BitStream *stream {nullptr};

    int width {0};
    int height {0};

    double frame_rate {0.0};
    double aspect_ratio {0.0};

    int bit_rate {0};

    uint8_t intra_quantizer_matrix[8][8];
    uint8_t non_intra_quantizer_matrix[8][8];

    int mb_width {0};
    int mb_height {0};

    Frame *frame_current {nullptr};
    Frame *frame_prev {nullptr};

    uint8_t picture_coding_type {0};

    unsigned int temporal_reference {0};

    int full_pel_forward_vector {0};
    int forward_r_size {0};
    int forward_f {0};

    int full_pel_backward_vector {0};
    int backward_r_size {0};
    int backward_f {0};

    int macroblock_address {-1};
    int past_intra_address {-2};

    unsigned int slice_vertical_position {0};

    // dct_dc_past[0] = dct_dc_y_past
    // dct_dc_past[1] = dct_dc_cb_past
    // dct_dc_past[2] = dct_dc_cr_past
    int dct_dc_past[3];

    bool first_mb_in_slice {false};

    int mb_row {0};
    int mb_col {0};

    int mb_type {0};

    bool macroblock_quant {false};
    bool macroblock_motion_forward {false};
    bool macroblock_motion_backward {false};
    bool macroblock_pattern {false};
    bool macroblock_intra {false};

    unsigned int quantizer_scale {0};

    int recon_right_for {0};
    int recon_down_for {0};

    int recon_right_for_prev {0};
    int recon_down_for_prev {0};

    int motion_horizontal_forward_code {0};
    int motion_horizontal_forward_r {0};

    int motion_vertical_forward_code {0};
    int motion_vertical_forward_r {0};

    int dct_zz[6][64];
    int dct_recon[6][64];

    int dct_dc_size_luminance {0};
    int dct_dc_size_chrominance {0};
    int dct_dc_differential {0};

    size_t current_picture_nr {0};

    queue<Mat *> *display_buffer {nullptr};
};