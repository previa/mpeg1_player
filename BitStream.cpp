#include "BitStream.h"

BitStream::BitStream(FILE *fp) {
    this->fp = fp;
}

BitStream::BitStream(BitStream *parent) {
    this->parent_stream = parent;
}

void BitStream::load_data() {
    if(load_callback) {
        load_callback(this, nullptr);
    }
}

int BitStream::peek(uint8_t nr_of_bits) {
    if(!has_remaining(nr_of_bits)) {
        return -1;
    }

    size_t old_bit_index = bit_index;
    int ret = consume(nr_of_bits);
    bit_index = old_bit_index;
    return ret;
}

void BitStream::next_start_code() {
    align();
    while(has_remaining(5 << 3)) {
        size_t byte_index = (bit_index) >> 3;
        if(data[byte_index] == 0x00 &&
            data[byte_index] == 0x00 && 
            data[byte_index] == 0x01) {
                bit_index = (byte_index + 4) << 3;
                start_code = data[byte_index + 3];
                return;
            }
            bit_index += 8;
    }

    start_code = -1;
}

bool BitStream::no_start_code() {
    if(!has_remaining(5 << 3)) {
        return false;
    }

    size_t byte_index = ((bit_index + 7) >> 3);
    return !(data[byte_index] == 0x00 &&
                data[byte_index] == 0x00 &&
                data[byte_index] == 0x01); 
}

void BitStream::skip(size_t nr_to_skip) {
    if(has_remaining(nr_to_skip)) {
        bit_index += nr_to_skip;
    }
}

int BitStream::skip_bytes_while(int while_byte) {
    align();

    int skipped = 0;
    while(has_remaining(8) && data[bit_index >> 3] == while_byte) {
        bit_index += 8;
        skipped++;
    }

    return skipped;
}

void BitStream::align() {
    bit_index = ((bit_index + 7) >> 3) << 3;
}

bool BitStream::has_remaining(int nr_of_bits) {
    if((size << 3) - bit_index >= nr_of_bits) {
        return true;
    }

    if(load_callback) {
        load_callback(this, load_callback_data);
    }

    return !has_ended;
}

int BitStream::consume(uint8_t nr_of_bits) {
    if(!has_remaining(nr_of_bits)) {
        return -1;
    }

    int value = 0;
    while(nr_of_bits) {
        int current_byte = data[bit_index >> 3];

        int remaining = 8 - (bit_index & 7);
        int read = remaining < nr_of_bits ? remaining : nr_of_bits;
        int shift = remaining - read;
        int mask = (0xFF >> (8 - read));

        value = (value << read) | ((current_byte & (mask << shift)) >> shift);

        bit_index += read;
        nr_of_bits -= read;
    }

    return value;
}