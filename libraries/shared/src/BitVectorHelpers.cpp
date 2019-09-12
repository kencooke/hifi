//
//  BitVectorHelpers.cpp
//  libraries/shared/src
//
//  Created by Ken Cooke on 4/22/19.
//  Copyright 2019 High Fidelity, Inc.
//
//  Distributed under the Apache License, Version 2.0.
//  See the accompanying file LICENSE or http://www.apache.org/licenses/LICENSE-2.0.html
//

#include "BitVectorHelpers.h"

#include <string.h>

class BitStream {

    uint8_t* _pos;      // current byte
    uint8_t* _start;    // start byte
    uint8_t* _end;      // one past final byte
    uint32_t _offset;   // bit offset

public:
    // create bitstream from existing buffer
    BitStream(uint8_t* buffer, int size, bool write) {
        assert(buffer != nullptr);
        assert(size != 0);

        _pos = buffer;
        _start = buffer;
        _end = buffer + size;
        _offset = 0;

        if (write) {
            // prepare bitstream for writing
            memset(buffer, 0, size);
        }
    }

    // write len bits to the bitstream
    void putBits(uint32_t bits, int len) {
        assert(_pos != nullptr);
        assert(&_pos[1] < _end);
        //assert(len <= 24);
        assert(len <= 8);

        uint32_t n = _offset + len;

        bits <<= 32 - n;
        _pos[0] = (uint8_t)((bits >> 24) | _pos[0]);
        _pos[1] = (uint8_t)(bits >> 16);
        //_pos[2] = (uint8_t)(bits >> 8);
        //_pos[3] = (uint8_t)(bits >> 0);

        _pos += n >> 3;
        _offset = n & 0x7;
    }

    // write single bit to the bitstream
    void putBit(uint8_t bit) {
        assert(_pos != nullptr);
        assert(&_pos[0] < _end);

        uint32_t n = _offset + 1;

        _pos[0] |= bit << (8 - n);

        _pos += n >> 3;
        _offset = n & 0x7;
    }

    // read len bits from the bitstream
    uint32_t getBits(int len) {
        assert(_pos != nullptr);
        assert(&_pos[0] < _end);
        //assert(len <= 24);
        assert(len <= 8);

        uint32_t n = _offset + len;

        uint32_t bits = (uint32_t)_pos[0] << 24;
        bits |= (uint32_t)_pos[1] << 16;
        //bits |= (uint32_t)_pos[2] << 8;
        //bits |= (uint32_t)_pos[3] << 0;
        bits <<= _offset;

        _pos += n >> 3;
        _offset = n & 0x7;

        return bits >> (32 - len);
    }

    // read single bit from the bitstream
    uint32_t getBit() {
        assert(_pos != nullptr);
        assert(&_pos[0] < _end);

        uint32_t n = _offset + 1;
        uint32_t bit = (uint32_t)_pos[0] >> (8 - n);

        _pos += n >> 3;
        _offset = n & 0x7;

        return bit & 0x1;
    }

    // flush bitstream, and return total bytes
    int flush() {
        assert(_pos != nullptr);

        // Pad the final byte with 1s, to ensure Golomb-Rice decoding
        // will detect out-of-bits without extraneous symbols.
        while (_offset) {
            putBit(1);
        }

        return _pos - _start;
    }

    // encode symbol using Golomb-Rice coding with parameter k
    void encodeGR(uint32_t symbol, int k) {
        assert(_pos != nullptr);
        assert(k >= 0);

        // prefix of q 1s terminated by 0
        uint32_t q = symbol >> k;
        while (q--) {
            putBit(1);
        }
        putBit(0);

        // remaining k bits
        if (k) {
            uint32_t r = symbol & ((1 << k) - 1);
            putBits(r, k);
        }
    }

    // decode symbol using Golomb-Rice coding with parameter k
    uint32_t decodeGR(int k) {
        assert(_pos != nullptr);
        assert(k >= 0);

        // count the run of 1s
        uint32_t q = 0;
        while (uint32_t bit = getBit()) {
            q++;
        }

        // remaining k bits
        if (k) {
            uint32_t r = getBits(k);
            q = (q << k) | r;
        }
        return q;
    }
};

static const int adaptive_k_table[2] = { 1, 5 };

int encodeBitVector2(bool* bitVector, int numBits, uint8_t* codeBytes, int maxCodeBytes) {
    assert(numBits > 0);
    assert(numBits <= MAX_BITVECTOR_BITS);

    int runLengths[MAX_BITVECTOR_BITS];
    BitStream bs(codeBytes, maxCodeBytes, true);

    for (int n = 0; n < 2; n++) {

        //
        // Run-length encode
        //
        bool startBit = bitVector[0];

        int numRunLengths = 0;
        int sumRunLengths = 0;
        for (int i = 0; i < numBits;) {

            bool bit = bitVector[i];
            int length = 0;

            while (++i < numBits && bitVector[i] == bit) {
                length++;
            }

            runLengths[numRunLengths++] = length;
            sumRunLengths += length;
        }

        // adapt k based on mean symbol value
        int ki = sumRunLengths > 8 * numRunLengths;
        int k = adaptive_k_table[ki];

        //
        // Encode the bits
        //

        // encode k index
        bs.putBit(ki);

        // encode start bit
        bs.putBit(startBit);

        // encode run lengths
        for (int i = 0; i < numRunLengths; i++) {
            bs.encodeGR(runLengths[i], k);        
        }

        bitVector += numBits;
    }

    // return bytes encoded
    return bs.flush();
}

void decodeBitVector2(uint8_t* codeBytes, int maxCodeBytes, bool* bitVector, int numBits) {
    assert(numBits > 0);
    assert(numBits <= MAX_BITVECTOR_BITS);

    int runLengths[MAX_BITVECTOR_BITS];
    BitStream bs(codeBytes, maxCodeBytes, false);

    for (int n = 0; n < 2; n++) {

        //
        // Decode the bits
        //

        // decode k index
        int ki = bs.getBit();
        int k = adaptive_k_table[ki];

        // decode start bit
        bool startBit = bs.getBit();

        // decode run lengths
        int numRunLengths = 0;
        int sumRunLengths = 0;
        while (numRunLengths + sumRunLengths < numBits) {

            uint32_t length = bs.decodeGR(k);

            runLengths[numRunLengths++] = length;
            sumRunLengths += length;
        }
        assert(numRunLengths + sumRunLengths == numBits);

        //
        // Run-length decode
        //
        bool bit = startBit;
        int numBitsDecoded = 0;
        for (int i = 0; i < numRunLengths; i++) {

            uint32_t length = runLengths[i];

            bitVector[numBitsDecoded++] = bit;
            while (length--) {
                bitVector[numBitsDecoded++] = bit;
            }

            bit ^= 0x1;
        }
        assert(numBitsDecoded == numBits);

        bitVector += numBits;
    }
}

#if 0   // unit test
#include <stdio.h>

int nextData(bool data[MAX_BITVECTOR_BITS]) {
    static FILE* file = fopen("bit-flag-data.txt", "r");

    // next bitVector string from Tony's dataset
    char str[MAX_BITVECTOR_BITS + 2];
    int numBits = 0;
    if (fgets(str, 256, file)) {

        numBits = strlen(str) - 1;
        assert(numBits <= MAX_BITVECTOR_BITS);

        for (int i = 0; i < numBits; i++) {
            data[i] = (str[i] == '1');
        }
    }
    return numBits;
}

void main(void) {

    int totalBitVectors = 0;
    int totalCodeBytes = 0;

    bool bitVector[2 * MAX_BITVECTOR_BITS]; // pair of bitVector[numBits]

    while (int numBits = nextData(&bitVector[0])) {

        // read second bitVector
        nextData(&bitVector[numBits]);

        // encode pair
        uint8_t codeBytes[MAX_CODE2_BYTES];
        int numCodeBytes = encodeBitVector2(bitVector, numBits, codeBytes, MAX_CODE2_BYTES);

        // decode pair
        bool bitVectorDecoded[2 * MAX_BITVECTOR_BITS];
        decodeBitVector2(codeBytes, MAX_CODE2_BYTES, bitVectorDecoded, numBits);

        // compare pair
        for (int i = 0; i < 2 * numBits; i++) {
            assert(bitVector[i] == bitVectorDecoded[i]);
        }

        // stats
        totalBitVectors += 2;
        totalCodeBytes += numCodeBytes;
        printf("bytes=%2d/%2d\n", numCodeBytes, 2 * ((numBits + 7) / 8));
    }
    printf("\nAverage bytes per vector = %0.2f\n", (double)totalCodeBytes / totalBitVectors);
}
#endif
