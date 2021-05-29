#include <cstdio>
#include <cstdint>
#include <cstdlib>
#include <cstring>

class BitStream;

typedef void (*stream_load_callback)(BitStream* self, void *data);

class BitStream {
public:
    BitStream(FILE*);
    BitStream(BitStream*);

    void load_data();
    int consume(uint8_t);
    bool has_remaining(int);
    void next_start_code();
    void align();
    int skip_bytes_while(int);
    void skip(size_t);
    bool no_start_code();
    int peek(uint8_t);

    BitStream* parent_stream;

    stream_load_callback load_callback {nullptr};
    void* load_callback_data {nullptr};

    FILE *fp {nullptr};

    size_t bit_index {0};
    size_t total_read {0};
    size_t size {0};
    size_t capacity {0};
    
    int start_code {0};
    int type {0};

    bool has_ended {false};

    uint8_t *data {nullptr};
};