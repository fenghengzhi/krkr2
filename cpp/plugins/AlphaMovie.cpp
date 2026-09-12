// AlphaMovie plugin. Four-file evidence and unresolved
// source names are recorded in analysis/alphamovie_reconstruction_four_binary_2026-09-13.md.
#include "tjsCommHead.h"
#include <cassert>
#include <cstdint>
#include <cstring>
#include <condition_variable>
#include <list>
#include <mutex>
#include <thread>
#include "PluginStub.h"
#include "ncbind.hpp"
#include "LayerIntf.h"
#include "LayerBitmapIntf.h"
#include "ThreadImpl.h"
#include "DetectCPU.h"

#define NCB_MODULE_NAME TJS_W("AlphaMovie.dll")

namespace {

const tjs_uint32 BitMasks_guess[33] = {
    0x00000000u, 0x00000001u, 0x00000003u, 0x00000007u, 0x0000000fu, 0x0000001fu,
    0x0000003fu, 0x0000007fu, 0x000000ffu, 0x000001ffu, 0x000003ffu, 0x000007ffu,
    0x00000fffu, 0x00001fffu, 0x00003fffu, 0x00007fffu, 0x0000ffffu, 0x0001ffffu,
    0x0003ffffu, 0x0007ffffu, 0x000fffffu, 0x001fffffu, 0x003fffffu, 0x007fffffu,
    0x00ffffffu, 0x01ffffffu, 0x03ffffffu, 0x07ffffffu, 0x0fffffffu, 0x1fffffffu,
    0x3fffffffu, 0x7fffffffu, 0xffffffffu,
};

const tjs_uint8 DCLumaBits_guess[17] = {
    0x00, 0x00, 0x01, 0x05, 0x01, 0x01, 0x01, 0x01, 0x01, 0x01, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
    0x00,
};

const tjs_uint8 ACLumaBits_guess[17] = {
    0x00, 0x00, 0x02, 0x01, 0x03, 0x03, 0x02, 0x04, 0x03, 0x05, 0x05, 0x04, 0x04, 0x00, 0x00, 0x01,
    0x7d,
};

const tjs_uint8 ACLumaValues_guess[162] = {
    0x01, 0x02, 0x03, 0x00, 0x04, 0x11, 0x05, 0x12, 0x21, 0x31, 0x41, 0x06, 0x13, 0x51, 0x61, 0x07,
    0x22, 0x71, 0x14, 0x32, 0x81, 0x91, 0xa1, 0x08, 0x23, 0x42, 0xb1, 0xc1, 0x15, 0x52, 0xd1, 0xf0,
    0x24, 0x33, 0x62, 0x72, 0x82, 0x09, 0x0a, 0x16, 0x17, 0x18, 0x19, 0x1a, 0x25, 0x26, 0x27, 0x28,
    0x29, 0x2a, 0x34, 0x35, 0x36, 0x37, 0x38, 0x39, 0x3a, 0x43, 0x44, 0x45, 0x46, 0x47, 0x48, 0x49,
    0x4a, 0x53, 0x54, 0x55, 0x56, 0x57, 0x58, 0x59, 0x5a, 0x63, 0x64, 0x65, 0x66, 0x67, 0x68, 0x69,
    0x6a, 0x73, 0x74, 0x75, 0x76, 0x77, 0x78, 0x79, 0x7a, 0x83, 0x84, 0x85, 0x86, 0x87, 0x88, 0x89,
    0x8a, 0x92, 0x93, 0x94, 0x95, 0x96, 0x97, 0x98, 0x99, 0x9a, 0xa2, 0xa3, 0xa4, 0xa5, 0xa6, 0xa7,
    0xa8, 0xa9, 0xaa, 0xb2, 0xb3, 0xb4, 0xb5, 0xb6, 0xb7, 0xb8, 0xb9, 0xba, 0xc2, 0xc3, 0xc4, 0xc5,
    0xc6, 0xc7, 0xc8, 0xc9, 0xca, 0xd2, 0xd3, 0xd4, 0xd5, 0xd6, 0xd7, 0xd8, 0xd9, 0xda, 0xe1, 0xe2,
    0xe3, 0xe4, 0xe5, 0xe6, 0xe7, 0xe8, 0xe9, 0xea, 0xf1, 0xf2, 0xf3, 0xf4, 0xf5, 0xf6, 0xf7, 0xf8,
    0xf9, 0xfa,
};

const tjs_uint8 DCChromaBits_guess[17] = {
    0x00, 0x00, 0x03, 0x01, 0x01, 0x01, 0x01, 0x01, 0x01, 0x01, 0x01, 0x01, 0x00, 0x00, 0x00, 0x00,
    0x00,
};

const tjs_uint8 ACChromaBits_guess[17] = {
    0x00, 0x00, 0x02, 0x01, 0x02, 0x04, 0x04, 0x03, 0x04, 0x07, 0x05, 0x04, 0x04, 0x00, 0x01, 0x02,
    0x77,
};

const tjs_uint8 ACChromaValues_guess[162] = {
    0x00, 0x01, 0x02, 0x03, 0x11, 0x04, 0x05, 0x21, 0x31, 0x06, 0x12, 0x41, 0x51, 0x07, 0x61, 0x71,
    0x13, 0x22, 0x32, 0x81, 0x08, 0x14, 0x42, 0x91, 0xa1, 0xb1, 0xc1, 0x09, 0x23, 0x33, 0x52, 0xf0,
    0x15, 0x62, 0x72, 0xd1, 0x0a, 0x16, 0x24, 0x34, 0xe1, 0x25, 0xf1, 0x17, 0x18, 0x19, 0x1a, 0x26,
    0x27, 0x28, 0x29, 0x2a, 0x35, 0x36, 0x37, 0x38, 0x39, 0x3a, 0x43, 0x44, 0x45, 0x46, 0x47, 0x48,
    0x49, 0x4a, 0x53, 0x54, 0x55, 0x56, 0x57, 0x58, 0x59, 0x5a, 0x63, 0x64, 0x65, 0x66, 0x67, 0x68,
    0x69, 0x6a, 0x73, 0x74, 0x75, 0x76, 0x77, 0x78, 0x79, 0x7a, 0x82, 0x83, 0x84, 0x85, 0x86, 0x87,
    0x88, 0x89, 0x8a, 0x92, 0x93, 0x94, 0x95, 0x96, 0x97, 0x98, 0x99, 0x9a, 0xa2, 0xa3, 0xa4, 0xa5,
    0xa6, 0xa7, 0xa8, 0xa9, 0xaa, 0xb2, 0xb3, 0xb4, 0xb5, 0xb6, 0xb7, 0xb8, 0xb9, 0xba, 0xc2, 0xc3,
    0xc4, 0xc5, 0xc6, 0xc7, 0xc8, 0xc9, 0xca, 0xd2, 0xd3, 0xd4, 0xd5, 0xd6, 0xd7, 0xd8, 0xd9, 0xda,
    0xe2, 0xe3, 0xe4, 0xe5, 0xe6, 0xe7, 0xe8, 0xe9, 0xea, 0xf2, 0xf3, 0xf4, 0xf5, 0xf6, 0xf7, 0xf8,
    0xf9, 0xfa,
};

const tjs_uint8 DCValues_guess[12] = {
    0x00, 0x01, 0x02, 0x03, 0x04, 0x05, 0x06, 0x07, 0x08, 0x09, 0x0a, 0x0b,
};

extern "C" const int jpeg_natural_order[];

class AlphaMovieBitReader_guess {
    const tjs_uint32 *Current;
    const tjs_uint8 *End;
    int Bits;
    tjs_uint32 Word;

    static tjs_uint32 Swap_guess(tjs_uint32 value) {
        return __builtin_bswap32(value);
    }

public:
    AlphaMovieBitReader_guess(const void *data, tjs_uint32 size) {
        const auto address = reinterpret_cast<std::uintptr_t>(data);
        const auto offset = address & 3;
        Current = reinterpret_cast<const tjs_uint32 *>(address - offset);
        End = static_cast<const tjs_uint8 *>(data) + size;
        Bits = 8 * (4 - offset);
        Word = Swap_guess(*Current);
    }

    tjs_uint32 PeekBits_guess(int count) const {
        if(count <= 0)
            return 0;
        const tjs_uint32 *current = Current;
        const tjs_uint8 *end = End;
        int bits = Bits;
        tjs_uint32 word = Word;
        tjs_uint32 result = 0;
        while(reinterpret_cast<const tjs_uint8 *>(current) < end) {
            const int remaining = bits - count;
            if(remaining >= 0)
                return result | (BitMasks_guess[count] & (word >> remaining));
            result |= (word & BitMasks_guess[bits]) << -remaining;
            count -= bits;
            word = Swap_guess(*++current);
            bits = 32;
            if(count <= 0)
                return result;
        }
        return result;
    }

    tjs_uint32 ReadBits_guess(int count) {
        if(count <= 0)
            return 0;
        tjs_uint32 result = 0;
        while(reinterpret_cast<const tjs_uint8 *>(Current) < End) {
            const int remaining = Bits - count;
            if(remaining >= 0) {
                result |= BitMasks_guess[count] & (Word >> remaining);
                Bits = remaining;
                // Exact exhaustion still prefetches the following word.
                if(remaining == 0) {
                    ++Current;
                    Bits = 32;
                    Word = Swap_guess(*Current);
                }
                return result;
            }
            result |= (Word & BitMasks_guess[Bits]) << -remaining;
            count -= Bits;
            ++Current;
            Bits = 32;
            Word = Swap_guess(*Current);
            if(count <= 0)
                return result;
        }
        return result;
    }

    void SkipBits_guess(int count) {
        Current += count >> 5;
        Bits -= count & 31;
        if(Bits <= 0) {
            ++Current;
            Bits += 32;
            Word = Swap_guess(*Current);
        }
    }
};

class AlphaMovieHuffman_guess {
protected:
    tjs_uint8 LookupBits[512];
    tjs_uint8 LookupSymbols[512];
    tjs_int16 LookupValues[512];
    const tjs_uint8 *Symbols;
    tjs_int32 MaxCode[18];
    tjs_int32 ValueOffset[17];

    int build_codes(tjs_uint16 *codes, const tjs_uint8 *bits) {
        int k = 0;
        int code = 0;
        for(int length = 1; length <= 16; ++length) {
            int nb = bits[length];
            assert(nb + k <= 256);
            while(nb-- > 0)
                codes[k++] = code++;
            code *= 2;
        }
        return k;
    }

public:
    AlphaMovieHuffman_guess(const tjs_uint8 *bits,
                           const tjs_uint8 *symbols) : Symbols(symbols) {
        tjs_uint16 codes[256];
        build_codes(codes, bits);
        int k = 0;
        for(int length = 1; length <= 16; ++length) {
            if(bits[length]) {
                ValueOffset[length] = k - codes[k];
                k += bits[length];
                MaxCode[length] = codes[k - 1];
            } else {
                MaxCode[length] = -1;
            }
        }
        MaxCode[17] = 0xfffff;
        // Only this table is cleared; the other lookup slots stay untouched.
        std::memset(LookupBits, 0, sizeof(LookupBits));
        k = 0;
        for(int length = 1; length < 9; ++length) {
            for(int j = 0; j < bits[length]; ++j, ++k) {
                int index = codes[k] << (9 - length);
                const int limit = index + (1 << (9 - length));
                const int symbol = Symbols[k];
                const int size = symbol & 15;
                const int shift = 9 - length - size;
                const int repeat = shift < 0 ? 1 : 1 << shift;
                const int mask = (1 << size) - 1;
                do {
                    int value = shift < 0 ? index << -shift : index >> shift;
                    value &= mask;
                    if(value < (1 << (size - 1)))
                        value += 1 - (1 << size);
                    for(int n = 0; n < repeat; ++n, ++index) {
                        LookupBits[index] = length + size;
                        LookupSymbols[index] = symbol;
                        LookupValues[index] = value;
                    }
                } while(index < limit);
            }
        }
    }

    int Decode1(AlphaMovieBitReader_guess &stream) {
        int length = 9;
        int code = stream.ReadBits_guess(9);
        while(code > MaxCode[length]) {
            code = (code * 2) | stream.ReadBits_guess(1);
            ++length;
        }
        assert(length <= 16);
        return Symbols[ValueOffset[length] + code];
    }
};

class AlphaMovieDCHuffman_guess : public AlphaMovieHuffman_guess {
    tjs_int32 Previous;

public:
    AlphaMovieDCHuffman_guess(const tjs_uint8 *bits,
                             const tjs_uint8 *symbols) :
        AlphaMovieHuffman_guess(bits, symbols), Previous(0) {}

    void Reset_guess() { Previous = 0; }

    int DecodeFirstValue(AlphaMovieBitReader_guess &stream) {
        const int index = stream.PeekBits_guess(9);
        const int bits = LookupBits[index];
        int value;
        if(bits) {
            value = LookupValues[index];
            if(bits <= 9) {
                stream.SkipBits_guess(bits);
            } else {
                stream.SkipBits_guess(9);
                value += stream.ReadBits_guess(bits - 9);
            }
        } else {
            const int size = Decode1(stream);
            assert(size > 0);
            value = stream.ReadBits_guess(size);
            if(value < (1 << (size - 1)))
                value += (-1 << size) + 1;
        }
        Previous += value;
        return Previous;
    }
};

class AlphaMovieACHuffman_guess : public AlphaMovieHuffman_guess {
public:
    using AlphaMovieHuffman_guess::AlphaMovieHuffman_guess;

    int DecodeAC(AlphaMovieBitReader_guess &stream, tjs_int16 *coefficients) {
        int index = 1;
        do {
            const int prefix = stream.PeekBits_guess(9);
            const int bits = LookupBits[prefix];
            int value;
            int run;
            if(bits) {
                value = LookupValues[prefix];
                run = LookupSymbols[prefix] >> 4;
                if(bits <= 9) {
                    stream.SkipBits_guess(bits);
                } else {
                    stream.SkipBits_guess(9);
                    value += stream.ReadBits_guess(bits - 9);
                }
            } else {
                const int symbol = Decode1(stream);
                assert(symbol > 0);
                run = symbol >> 4;
                const int size = symbol & 15;
                value = stream.ReadBits_guess(size);
                if(value < (1 << (size - 1)))
                    value += (-1 << size) + 1;
            }
            if(value != 0) {
                index += run;
                coefficients[jpeg_natural_order[index]] = value;
            } else {
                if(run != 15)
                    return index;
                index += 15;
            }
            ++index;
        } while(index < 64);
        return index;
    }
};

} // namespace

extern "C" {
#include <jpeglib.h>
#include <libswscale/swscale.h>
#include <zlib.h>
#if defined(__ANDROID__) && (defined(__arm__) || defined(__aarch64__)) && \
    defined(WITH_SIMD)
void jsimd_idct_islow_neon(void *, JCOEFPTR, JSAMPARRAY, JDIMENSION);
#endif
}

#include "tjsUtils.h"
#include "StorageIntf.h"
#include "DebugIntf.h"
#include "MsgIntf.h"
#include "tvpgl.h"
#include "movie/ffmpeg/krffmpeg.h"

namespace {


// The reference JPEG kernels consume signed 16-bit multipliers. The host
// Wasm JPEG library uses 32-bit multipliers, so retain this 8x8 kernel locally.
#if defined(__APPLE__)
using AlphaMovieJPEGInt_guess = long;
#else
using AlphaMovieJPEGInt_guess = tjs_int32;
#endif

void jpeg_idct_islow(j_decompress_ptr info, jpeg_component_info *component,
                    JCOEFPTR coefficients, JSAMPARRAY rows,
                    JDIMENSION outputColumn) {
    AlphaMovieJPEGInt_guess tmp0, tmp1, tmp2, tmp3;
    AlphaMovieJPEGInt_guess tmp10, tmp11, tmp12, tmp13;
    AlphaMovieJPEGInt_guess z1, z2, z3, z4, z5;
    int workspace[64];
    auto *range = info->sample_range_limit + 128;
    auto *quant = static_cast<tjs_int16 *>(component->dct_table);
    auto *input = coefficients;
    int *work = workspace;

    for(int col = 0; col < 8; ++col, ++input, ++quant, ++work) {
        if(input[8] == 0 && input[16] == 0 && input[24] == 0 &&
           input[32] == 0 && input[40] == 0 && input[48] == 0 &&
           input[56] == 0) {
            int dc = (input[0] * quant[0]) << 2;
            work[0] = dc;
            work[8] = dc;
            work[16] = dc;
            work[24] = dc;
            work[32] = dc;
            work[40] = dc;
            work[48] = dc;
            work[56] = dc;
            continue;
        }
        z2 = input[16] * quant[16];
        z3 = input[48] * quant[48];
        z1 = (z2 + z3) * 4433;
        tmp2 = z1 - z3 * 15137;
        tmp3 = z1 + z2 * 6270;

        z2 = input[0] * quant[0];
        z3 = input[32] * quant[32];
        tmp0 = (z2 + z3) << 13;
        tmp1 = (z2 - z3) << 13;
        tmp10 = tmp0 + tmp3;
        tmp13 = tmp0 - tmp3;
        tmp11 = tmp1 + tmp2;
        tmp12 = tmp1 - tmp2;

        tmp0 = input[56] * quant[56];
        tmp1 = input[40] * quant[40];
        tmp2 = input[24] * quant[24];
        tmp3 = input[8] * quant[8];
        z1 = tmp0 + tmp3;
        z2 = tmp1 + tmp2;
        z3 = tmp0 + tmp2;
        z4 = tmp1 + tmp3;
        z5 = (z3 + z4) * 9633;
        tmp0 *= 2446;
        tmp1 *= 16819;
        tmp2 *= 25172;
        tmp3 *= 12299;
        z1 *= -7373;
        z2 *= -20995;
        z3 *= -16069;
        z4 *= -3196;
        z3 += z5;
        z4 += z5;
        tmp0 += z1 + z3;
        tmp1 += z2 + z4;
        tmp2 += z2 + z3;
        tmp3 += z1 + z4;

        work[0] = (tmp10 + tmp3 + 1024) >> 11;
        work[56] = (tmp10 - tmp3 + 1024) >> 11;
        work[8] = (tmp11 + tmp2 + 1024) >> 11;
        work[48] = (tmp11 - tmp2 + 1024) >> 11;
        work[16] = (tmp12 + tmp1 + 1024) >> 11;
        work[40] = (tmp12 - tmp1 + 1024) >> 11;
        work[24] = (tmp13 + tmp0 + 1024) >> 11;
        work[32] = (tmp13 - tmp0 + 1024) >> 11;
    }

    work = workspace;
    for(int row = 0; row < 8; ++row, work += 8) {
        auto *output = rows[row] + outputColumn;
        if(work[1] == 0 && work[2] == 0 && work[3] == 0 &&
           work[4] == 0 && work[5] == 0 && work[6] == 0 && work[7] == 0) {
            auto dc = range[((work[0] + 16) >> 5) & 1023];
            output[0] = dc;
            output[1] = dc;
            output[2] = dc;
            output[3] = dc;
            output[4] = dc;
            output[5] = dc;
            output[6] = dc;
            output[7] = dc;
            continue;
        }

        z2 = work[2];
        z3 = work[6];
        z1 = (z2 + z3) * 4433;
        tmp2 = z1 - z3 * 15137;
        tmp3 = z1 + z2 * 6270;
        tmp0 = (static_cast<AlphaMovieJPEGInt_guess>(work[0]) + work[4]) << 13;
        tmp1 = (static_cast<AlphaMovieJPEGInt_guess>(work[0]) - work[4]) << 13;
        tmp10 = tmp0 + tmp3;
        tmp13 = tmp0 - tmp3;
        tmp11 = tmp1 + tmp2;
        tmp12 = tmp1 - tmp2;

        tmp0 = work[7];
        tmp1 = work[5];
        tmp2 = work[3];
        tmp3 = work[1];
        z1 = tmp0 + tmp3;
        z2 = tmp1 + tmp2;
        z3 = tmp0 + tmp2;
        z4 = tmp1 + tmp3;
        z5 = (z3 + z4) * 9633;
        tmp0 *= 2446;
        tmp1 *= 16819;
        tmp2 *= 25172;
        tmp3 *= 12299;
        z1 *= -7373;
        z2 *= -20995;
        z3 *= -16069;
        z4 *= -3196;
        z3 += z5;
        z4 += z5;
        tmp0 += z1 + z3;
        tmp1 += z2 + z4;
        tmp2 += z2 + z3;
        tmp3 += z1 + z4;

        output[0] = range[((tmp10 + tmp3 + 131072) >> 18) & 1023];
        output[7] = range[((tmp10 - tmp3 + 131072) >> 18) & 1023];
        output[1] = range[((tmp11 + tmp2 + 131072) >> 18) & 1023];
        output[6] = range[((tmp11 - tmp2 + 131072) >> 18) & 1023];
        output[2] = range[((tmp12 + tmp1 + 131072) >> 18) & 1023];
        output[5] = range[((tmp12 - tmp1 + 131072) >> 18) & 1023];
        output[3] = range[((tmp13 + tmp0 + 131072) >> 18) & 1023];
        output[4] = range[((tmp13 - tmp0 + 131072) >> 18) & 1023];
    }
}


void AlphaMovieIDCTScalar_guess(tjs_int16 *quantization,
                                tjs_int16 *coefficients,
                                tjs_uint8 *pixels, int pitch) {
    JSAMPROW rows[8];
    for(int i = 0; i < 8; ++i)
        rows[i] = pixels + i * pitch;
    static JSAMPLE *range = [] {
        auto *storage = new JSAMPLE[1408];
        std::memset(storage, 0, 256);
        for(int i = 0; i < 256; ++i)
            storage[256 + i] = i;
        std::memset(storage + 512, 255, 384);
        std::memset(storage + 896, 0, 384);
        std::memcpy(storage + 1280, storage + 256, 128);
        return storage + 256;
    }();
    jpeg_decompress_struct info;
    jpeg_component_info component;
    info.sample_range_limit = range;
    component.dct_table = quantization;
    jpeg_idct_islow(&info, &component, coefficients, rows, 0);
}

using AlphaMovieIDCTProc_guess =
    void (*)(tjs_int16 *, tjs_int16 *, tjs_uint8 *, int);
AlphaMovieIDCTProc_guess AlphaMovieIDCT_guess = AlphaMovieIDCTScalar_guess;

struct AlphaMovieFileHeader_guess {
    tjs_uint32 Magic;
    tjs_uint32 FileSize_guess;
    tjs_uint32 Revision;
    tjs_uint32 HeaderSize;
    tjs_uint32 Unused_guess;
    tjs_uint32 NumOfFrame;
    tjs_uint32 FPSScale;
    tjs_uint32 FPSRate;
    tjs_uint16 Width;
    tjs_uint16 Height;
    tjs_uint8 Attribute;
};

struct AlphaMovieFrameHeader_guess {
    tjs_uint32 Magic;
    tjs_uint32 Size;
    tjs_uint32 Frame;
    tjs_uint16 Left;
    tjs_uint16 Top;
    tjs_uint16 Width;
    tjs_uint16 Height;
};

struct AlphaMovieSeparateFrameHeader_guess : AlphaMovieFrameHeader_guess {
    tjs_uint32 AlphaSize;
};

class AlphaMovieBuffer_guess {
    tjs_uint8 *Buffer;

public:
    explicit AlphaMovieBuffer_guess(tjs_uint32 size) :
        Buffer(static_cast<tjs_uint8 *>(TJSAlignedAlloc(size, 4))) {}
    ~AlphaMovieBuffer_guess() {
        if(Buffer)
            TJSAlignedDealloc(Buffer);
    }
    tjs_uint8 *Get_guess() const { return Buffer; }
};

struct AlphaMovieFrame_guess {
    tjs_uint32 *Pixels;
    tjs_int32 Left;
    tjs_int32 Top;
    tjs_int32 Width;
    tjs_int32 Height;
    tjs_int32 Pitch;
    tjs_int32 Frame;

    AlphaMovieFrame_guess() :
        Pixels(nullptr), Left(0), Top(0), Width(0), Height(0) {}

    ~AlphaMovieFrame_guess() {
        if(Pixels)
            TJSAlignedDealloc(Pixels);
    }

    void Resize_guess(int left, int top, int width, int height) {
        const int pitch = (width + 15) & ~15;
        if(pitch != Pitch || height != Height) {
            if(Pixels)
                TJSAlignedDealloc(Pixels);
            Pixels = nullptr;
        }
        Height = height;
        Pitch = pitch;
        Left = left;
        Top = top;
        Width = width;
        // The allocation uses width, while the decoded planes use pitch.
        if(!Pixels)
            Pixels = static_cast<tjs_uint32 *>(
                TJSAlignedAlloc(4 * width * (height + 1), 4));
    }
};

class AlphaMovieDecoder_guess {
    tjs_int16 Quantization[3][64];
    tTJSBinaryStream *Stream;
    AlphaMovieDCHuffman_guess DCLuma;
    AlphaMovieACHuffman_guess ACLuma;
    AlphaMovieDCHuffman_guess DCChroma;
    AlphaMovieACHuffman_guess ACChroma;
    SwsContext *ScaleContext;

public:
    tjs_uint32 Frame;
    tjs_uint32 NumOfFrame;
    tjs_uint32 FPSScale;
    tjs_uint32 FPSRate;
    tjs_int32 ScreenWidth;
    tjs_int32 ScreenHeight;
    tjs_uint32 FirstPosition;
    tjs_uint32 Position;
    bool SeparateAlpha;
    bool Loop;
    bool NextLoop;
    bool ReservedFlag_guess;
    ttstr NextMovieFile;

    AlphaMovieDecoder_guess() :
        Stream(nullptr), DCLuma(DCLumaBits_guess, DCValues_guess),
        ACLuma(ACLumaBits_guess, ACLumaValues_guess),
        DCChroma(DCChromaBits_guess, DCValues_guess),
        ACChroma(ACChromaBits_guess, ACChromaValues_guess),
        ScaleContext(nullptr), Loop(true), NextLoop(true),
        ReservedFlag_guess(false) {
        TVPInitLibAVCodec();
    }

    ~AlphaMovieDecoder_guess() {
        if(Stream)
            delete Stream;
        if(ScaleContext)
            sws_freeContext(ScaleContext);
    }

    bool Open_guess(const ttstr &name) {
        if(Stream)
            delete Stream;
        Stream = TVPCreateStream(name, TJS_BS_READ);
        AlphaMovieFileHeader_guess header;
        tjs_uint8 tables[192];
        tjs_uint32 tableSize;
        if(!Stream) {
            TVPAddLog(ttstr("Alpha Movie : ") + TJS_W("File open faild."));
            goto failed;
        }
        if(Stream->Read(&header, 40) != 40) {
            TVPAddLog(ttstr("Alpha Movie : ") + TJS_W("File read error"));
            goto failed;
        }
        if(header.Magic != 1297107521u) {
            TVPAddLog(ttstr("Alpha Movie : ") +
                      TJS_W("This file is not Alpha Movie File."));
            goto failed;
        }
        if(header.Revision != 0) {
            TVPAddLog(ttstr("Alpha Movie : ") +
                      TJS_W("Invalid File revision number."));
            goto failed;
        }
        if(header.HeaderSize != 168 && header.HeaderSize != 232) {
            TVPAddLog(ttstr("Alpha Movie : ") +
                      TJS_W("Invalid Quantaization table size."));
            goto failed;
        }
        NumOfFrame = header.NumOfFrame;
        if(NumOfFrame == 0) {
            TVPAddLog(ttstr("Alpha Movie : ") +
                      TJS_W("Not found frame in this file."));
            goto failed;
        }
        FPSScale = header.FPSScale;
        if(FPSScale == 0) {
            TVPAddLog(ttstr("Alpha Movie : ") + TJS_W("Invalid frame rate."));
            goto failed;
        }
        FPSRate = header.FPSRate;
        if(FPSRate == 0) {
            TVPAddLog(ttstr("Alpha Movie : ") + TJS_W("Invalid frame rate."));
            goto failed;
        }
        ScreenWidth = header.Width;
        if(ScreenWidth == 0) {
            TVPAddLog(ttstr("Alpha Movie : ") + TJS_W("Screen size is zero ?"));
            goto failed;
        }
        ScreenHeight = header.Height;
        if(ScreenHeight == 0) {
            TVPAddLog(ttstr("Alpha Movie : ") + TJS_W("Screen size is zero ?"));
            goto failed;
        }
        tableSize = header.HeaderSize - 40;
        if(header.Attribute & 1) {
            SeparateAlpha = false;
            if(header.HeaderSize != 232) {
                TVPAddLog(ttstr("Alpha Movie : ") +
                          TJS_W("Invalid Quantaization table size."));
                goto failed;
            }
        } else {
            if(!(header.Attribute & 2)) {
                TVPAddLog(ttstr("Alpha Movie : ") + TJS_W("Invalid Attribute."));
                goto failed;
            }
            SeparateAlpha = true;
            if(header.HeaderSize != 168) {
                TVPAddLog(ttstr("Alpha Movie : ") +
                          TJS_W("Invalid Quantaization table size."));
                goto failed;
            }
        }
        if(Stream->Read(tables, tableSize) != tableSize) {
            TVPAddLog(ttstr("Alpha Movie : ") + TJS_W("File read error"));
            goto failed;
        }
        FirstPosition = Stream->GetPosition();
        Position = FirstPosition;
        Frame = 0;
        for(int i = 0; i < 64; ++i)
            Quantization[0][i] = tables[i];
        for(int i = 0; i < 64; ++i)
            Quantization[1][i] = tables[64 + i];
        if(tableSize == 128) {
            std::memset(Quantization[2], 0, sizeof(Quantization[2]));
        } else {
            for(int i = 0; i < 64; ++i)
                Quantization[2][i] = tables[128 + i];
        }
        return true;

    failed:
        if(Stream) {
            delete Stream;
            Stream = nullptr;
        }
        return false;
    }

    int DecodeNext_guess(AlphaMovieFrame_guess *sample) {
        if(Frame >= NumOfFrame - 1) {
            if(Loop) {
                Position = FirstPosition;
                Frame = 0;
            } else {
                if(NextMovieFile.IsEmpty() || !Open_guess(NextMovieFile))
                    return 2;
                Loop = NextLoop;
            }
        }
        if(Stream->Seek(Position, TJS_BS_SEEK_SET) != Position) {
            TVPAddLog(TJS_W("Cannot seek."));
            return 1;
        }
        if(SeparateAlpha)
            return DecodeSeparateAlpha_guess(sample);
        return DecodeBlockAlpha_guess(sample);
    }

    bool SeekFrame_guess(tjs_uint32 frame) {
        if(frame >= NumOfFrame) {
            TVPAddLog(TJS_W("out of movie."));
            return false;
        }
        tjs_int32 position = FirstPosition;
        if(Stream->Seek(position, TJS_BS_SEEK_SET) !=
           static_cast<tjs_uint64>(static_cast<tjs_int64>(position))) {
            TVPAddLog(TJS_W("Cannot seek."));
            return false;
        }
        for(;;) {
            AlphaMovieFrameHeader_guess header;
            if(Stream->Read(&header, 20) != 20) {
                TVPAddLog(TJS_W("Read seek."));
                return false;
            }
            if(header.Magic != 1296126534u) {
                TVPAddLog(TJS_W("File format error."));
                return false;
            }
            if(header.Frame == frame) {
                Frame = frame;
                Position = position;
                return true;
            }
            if(header.Frame > frame) {
                TVPAddLog(TJS_W("No found frame."));
                return false;
            }
            position += header.Size + 8;
            if(Stream->Seek(position, TJS_BS_SEEK_SET) !=
               static_cast<tjs_uint64>(static_cast<tjs_int64>(position))) {
                TVPAddLog(TJS_W("Cannot seek."));
                return false;
            }
        }
    }

private:
    void DecodeChromaBlock_guess(AlphaMovieBitReader_guess &stream,
                                 tjs_int16 *quantization,
                                 tjs_uint8 *pixels, int pitch) {
        tjs_uint8 buffer[144];
        std::memset(buffer, 0, sizeof(buffer));
        auto *coefficients = reinterpret_cast<tjs_int16 *>(
            (reinterpret_cast<std::uintptr_t>(buffer) + 15) &
            ~std::uintptr_t(15));
        coefficients[0] = DCChroma.DecodeFirstValue(stream);
        if(ACChroma.DecodeAC(stream, coefficients) == 1) {
            int value = coefficients[0] * quantization[0] / 8 + 128;
            if(value > 255)
                value = 255;
            else if(value <= 0)
                value = 0;
            const tjs_uint64 row = tjs_uint64(0x0101010101010101ULL) *
                static_cast<tjs_uint8>(value);
            for(int i = 0; i < 8; ++i) {
                *reinterpret_cast<tjs_uint64 *>(pixels) = row;
                pixels += pitch;
            }
        } else {
            AlphaMovieIDCT_guess(quantization, coefficients, pixels, pitch);
        }
    }

    void DecodeLumaBlock_guess(AlphaMovieBitReader_guess &stream,
                               tjs_int16 *quantization,
                               tjs_uint8 *pixels, int pitch) {
        tjs_uint8 buffer[144];
        std::memset(buffer, 0, sizeof(buffer));
        auto *coefficients = reinterpret_cast<tjs_int16 *>(
            (reinterpret_cast<std::uintptr_t>(buffer) + 15) &
            ~std::uintptr_t(15));
        coefficients[0] = DCLuma.DecodeFirstValue(stream);
        if(ACLuma.DecodeAC(stream, coefficients) == 1) {
            int value = coefficients[0] * quantization[0] / 8 + 128;
            if(value > 255)
                value = 255;
            else if(value <= 0)
                value = 0;
            const tjs_uint64 row = tjs_uint64(0x0101010101010101ULL) *
                static_cast<tjs_uint8>(value);
            for(int i = 0; i < 8; ++i) {
                *reinterpret_cast<tjs_uint64 *>(pixels) = row;
                pixels += pitch;
            }
        } else {
            AlphaMovieIDCT_guess(quantization, coefficients, pixels, pitch);
        }
    }

    int DecodeSeparateAlpha_guess(AlphaMovieFrame_guess *sample) {
        AlphaMovieSeparateFrameHeader_guess header;
        if(Stream->Read(&header, 24) != 24)
            return 1;
        sample->Resize_guess(header.Left, header.Top, header.Width,
                             header.Height);
        sample->Frame = header.Frame;
        const int halfPitch = sample->Pitch / 2;
        AlphaMovieBuffer_guess y(sample->Height * sample->Pitch);
        AlphaMovieBuffer_guess firstChroma(sample->Height * sample->Pitch / 4);
        AlphaMovieBuffer_guess secondChroma(sample->Height * sample->Pitch / 4);
        AlphaMovieBuffer_guess alpha(sample->Height * sample->Pitch);
        {
            AlphaMovieBuffer_guess compressed(header.AlphaSize);
            if(Stream->Read(compressed.Get_guess(), header.AlphaSize) !=
               header.AlphaSize)
                return 1;
            uLongf size = header.Width * header.Height;
            uncompress(alpha.Get_guess(), &size, compressed.Get_guess(),
                       header.AlphaSize);
        }
        const tjs_uint32 dataSize = header.Size - header.AlphaSize - 16;
        AlphaMovieBuffer_guess data(dataSize);
        if(Stream->Read(data.Get_guess(), dataSize) != dataSize)
            return 1;
        Position += header.Size + 8;
        Frame = header.Frame;
        AlphaMovieBitReader_guess stream(data.Get_guess(), dataSize);
        DCLuma.Reset_guess();
        DCChroma.Reset_guess();
        ScaleContext = sws_getCachedContext(
            ScaleContext, sample->Width, sample->Height, AV_PIX_FMT_YUVA420P,
            sample->Width, sample->Height, AV_PIX_FMT_BGRA, SWS_FAST_BILINEAR,
            nullptr, nullptr, nullptr);
        int *invTable;
        int *table;
        int srcRange, dstRange, brightness, contrast, saturation;
        sws_getColorspaceDetails(ScaleContext, &invTable, &srcRange, &table,
                                 &dstRange, &brightness, &contrast, &saturation);
        int invCopy[4];
        int tableCopy[4];
        std::memcpy(invCopy, invTable, sizeof(invCopy));
        std::memcpy(tableCopy, table, sizeof(tableCopy));
        srcRange = dstRange = 1;
        sws_setColorspaceDetails(ScaleContext, invCopy, srcRange, tableCopy,
                                 dstRange, brightness, contrast, saturation);
        int chromaRow = 0;
        int lumaRow = 0;
        for(int row = 0; row < header.Height / 16; ++row) {
            tjs_uint8 *yp = y.Get_guess() + sample->Pitch * lumaRow;
            tjs_uint8 *cp1 = firstChroma.Get_guess() + chromaRow;
            tjs_uint8 *cp2 = secondChroma.Get_guess() + chromaRow;
            for(int col = 0; col < header.Width / 16; ++col) {
                DecodeChromaBlock_guess(stream, Quantization[1], cp1, halfPitch);
                DecodeChromaBlock_guess(stream, Quantization[1], cp2, halfPitch);
                DecodeLumaBlock_guess(stream, Quantization[0], yp, sample->Pitch);
                DecodeLumaBlock_guess(stream, Quantization[0], yp + 8,
                                       sample->Pitch);
                DecodeLumaBlock_guess(stream, Quantization[0],
                                       yp + 8 * sample->Pitch, sample->Pitch);
                DecodeLumaBlock_guess(stream, Quantization[0],
                                       yp + 8 * sample->Pitch + 8, sample->Pitch);
                yp += 16;
                cp1 += 8;
                cp2 += 8;
            }
            chromaRow += 8 * halfPitch;
            lumaRow += 16;
        }
        const tjs_uint8 *planes[4] = {
            y.Get_guess(), secondChroma.Get_guess(), firstChroma.Get_guess(),
            alpha.Get_guess()};
        int strides[4] = {sample->Pitch, sample->Pitch / 2, sample->Pitch / 2,
                          sample->Pitch};
        tjs_uint8 *output[4] = {
            reinterpret_cast<tjs_uint8 *>(sample->Pixels), nullptr, nullptr,
            nullptr};
        int outputStrides[4] = {4 * sample->Pitch, 0, 0, 0};
        sws_scale(ScaleContext, planes, strides, 0, sample->Height, output,
                  outputStrides);
        TVPBindMaskToMain(sample->Pixels, alpha.Get_guess(),
                         sample->Height * sample->Pitch);
        return 0;
    }

    int DecodeBlockAlpha_guess(AlphaMovieFrame_guess *sample) {
        AlphaMovieFrameHeader_guess header;
        if(Stream->Read(&header, 20) != 20)
            return 1;
        const tjs_uint32 dataSize = header.Size - 12;
        AlphaMovieBuffer_guess data(dataSize);
        if(Stream->Read(data.Get_guess(), dataSize) != dataSize)
            return 1;
        Position += header.Size + 8;
        Frame = header.Frame;
        AlphaMovieBitReader_guess stream(data.Get_guess(), dataSize);
        DCLuma.Reset_guess();
        DCChroma.Reset_guess();
        sample->Resize_guess(header.Left, header.Top, header.Width,
                             header.Height);
        sample->Frame = header.Frame;
        const int halfPitch = sample->Pitch / 2;
        if(header.Width != 0 && header.Height != 0) {
            AlphaMovieBuffer_guess y(sample->Height * sample->Pitch);
            AlphaMovieBuffer_guess firstChroma(
                sample->Height * sample->Pitch / 4);
            AlphaMovieBuffer_guess secondChroma(
                sample->Height * sample->Pitch / 4);
            AlphaMovieBuffer_guess alpha(sample->Height * sample->Pitch);
            ScaleContext = sws_getCachedContext(
                ScaleContext, sample->Width, sample->Height,
                AV_PIX_FMT_YUVA420P, sample->Width, sample->Height,
                AV_PIX_FMT_BGRA, SWS_FAST_BILINEAR, nullptr, nullptr, nullptr);
            int *invTable;
            int *table;
            int srcRange, dstRange, brightness, contrast, saturation;
            sws_getColorspaceDetails(ScaleContext, &invTable, &srcRange, &table,
                                     &dstRange, &brightness, &contrast,
                                     &saturation);
            int invCopy[4];
            int tableCopy[4];
            std::memcpy(invCopy, invTable, sizeof(invCopy));
            std::memcpy(tableCopy, table, sizeof(tableCopy));
            srcRange = dstRange = 1;
            sws_setColorspaceDetails(ScaleContext, invCopy, srcRange, tableCopy,
                                     dstRange, brightness, contrast, saturation);
            int chromaRow = 0;
            int lumaRow = 0;
            for(int row = 0; row < header.Height / 16; ++row) {
                tjs_uint8 *yp = y.Get_guess() + sample->Pitch * lumaRow;
                tjs_uint8 *ap = alpha.Get_guess() + sample->Pitch * lumaRow;
                tjs_uint8 *cp1 = firstChroma.Get_guess() + chromaRow;
                tjs_uint8 *cp2 = secondChroma.Get_guess() + chromaRow;
                for(int col = 0; col < header.Width / 16; ++col) {
                    DecodeChromaBlock_guess(stream, Quantization[1], cp1,
                                             halfPitch);
                    DecodeChromaBlock_guess(stream, Quantization[1], cp2,
                                             halfPitch);
                    DecodeLumaBlock_guess(stream, Quantization[0], yp,
                                           sample->Pitch);
                    DecodeLumaBlock_guess(stream, Quantization[0], yp + 8,
                                           sample->Pitch);
                    DecodeLumaBlock_guess(stream, Quantization[0],
                                           yp + 8 * sample->Pitch, sample->Pitch);
                    DecodeLumaBlock_guess(stream, Quantization[0],
                                           yp + 8 * sample->Pitch + 8,
                                           sample->Pitch);
                    DecodeLumaBlock_guess(stream, Quantization[2], ap,
                                           sample->Pitch);
                    DecodeLumaBlock_guess(stream, Quantization[2], ap + 8,
                                           sample->Pitch);
                    DecodeLumaBlock_guess(stream, Quantization[2],
                                           ap + 8 * sample->Pitch, sample->Pitch);
                    DecodeLumaBlock_guess(stream, Quantization[2],
                                           ap + 8 * sample->Pitch + 8,
                                           sample->Pitch);
                    yp += 16;
                    ap += 16;
                    cp1 += 8;
                    cp2 += 8;
                }
                chromaRow += 8 * halfPitch;
                lumaRow += 16;
            }
            const tjs_uint8 *planes[4] = {
                y.Get_guess(), secondChroma.Get_guess(), firstChroma.Get_guess(),
                alpha.Get_guess()};
            int strides[4] = {sample->Pitch, sample->Pitch / 2,
                              sample->Pitch / 2, sample->Pitch};
            tjs_uint8 *output[4] = {
                reinterpret_cast<tjs_uint8 *>(sample->Pixels), nullptr,
                nullptr, nullptr};
            int outputStrides[4] = {4 * sample->Pitch, 0, 0, 0};
            sws_scale(ScaleContext, planes, strides, 0, sample->Height, output,
                      outputStrides);
            TVPBindMaskToMain(sample->Pixels, alpha.Get_guess(),
                             sample->Height * sample->Pitch);
        }
        return 0;
    }
};

} // namespace


namespace {

class AlphaMovieCondition_guess {
    std::condition_variable Condition;
    std::mutex Mutex;

public:
    void Wait_guess() {
        std::unique_lock<std::mutex> lock(Mutex);
        Condition.wait(lock);
    }
    void Notify_guess() { Condition.notify_one(); }
};

class AlphaMovieNative_guess : public tTJSNativeInstance {
    tTJSCriticalSection QueueLock;
    tTJSCriticalSection ReservedLock_guess;
    tTJSCriticalSection StopLock;

public:
    AlphaMovieDecoder_guess Decoder;

private:
    std::thread *Thread;
    AlphaMovieCondition_guess FreeCondition;
    AlphaMovieCondition_guess ReadyCondition;
    std::list<AlphaMovieFrame_guess *> Ready;
    std::list<AlphaMovieFrame_guess *> Free;
    tjs_int32 DisplayedFrame;

public:
    tjs_int32 Left;
    tjs_int32 Top;
    tjs_int32 PreloadSamples;

private:
    bool FrameNotified_guess;
    bool Stop;
    bool Exit;

public:
    static void *operator new(std::size_t size) {
        return TJSAlignedAlloc(size, 4);
    }
    static void operator delete(void *instance) {
        TJSAlignedDealloc(instance);
    }

    AlphaMovieNative_guess() :
        Thread(nullptr), DisplayedFrame(0), Left(0), Top(0), PreloadSamples(5),
        FrameNotified_guess(false), Stop(false), Exit(false) {}

    ~AlphaMovieNative_guess() override {
        clear();
        stop();
    }

    void open(const ttstr &name) {
        if(!Decoder.Open_guess(name))
            TVPThrowExceptionMessage(
                (TJS_W("can't open alpha movie file - ") + name).c_str());
    }

    void clear() {
        tTJSCSH lock(QueueLock);
        for(auto *sample : Ready)
            delete sample;
        Ready.clear();
        for(auto *sample : Free)
            delete sample;
        Free.clear();
    }

    bool isPlaying() {
        bool stopped;
        {
            tTJSCSH lock(StopLock);
            stopped = Stop;
        }
        return !stopped;
    }

    void stop() {
        {
            tTJSCSH lock(StopLock);
            Stop = true;
            Exit = true;
        }
        FreeCondition.Notify_guess();
        if(Thread) {
            Thread->join();
            delete Thread;
            Thread = nullptr;
        }
    }

    void play() {
        stop();
        tTJSCSH lock(QueueLock);
        Decoder.Position = Decoder.FirstPosition;
        Decoder.Frame = 0;
        for(auto *sample : Ready)
            Free.push_back(sample);
        Ready.clear();
        while(Free.size() > static_cast<std::size_t>(PreloadSamples)) {
            delete Free.front();
            Free.pop_front();
        }
        while(Free.size() < static_cast<std::size_t>(PreloadSamples))
            Free.push_back(new AlphaMovieFrame_guess);
        Stop = false;
        Exit = false;
        Thread = new std::thread(&AlphaMovieNative_guess::Worker_guess, this);
    }

    void setPosition(tjs_int32 left, tjs_int32 top) {
        Left = left;
        Top = top;
    }

    void setNextMovieFile(const ttstr &name) {
        Decoder.NextMovieFile = name;
    }

    void SetFrame_guess(tjs_uint32 frame) {
        tTJSCSH lock(QueueLock);
        for(auto *sample : Ready)
            Free.push_back(sample);
        Ready.clear();
        if(Decoder.SeekFrame_guess(frame)) {
            FrameNotified_guess = true;
            FreeCondition.Notify_guess();
        }
    }

    tjs_int32 showNextImage(iTJSDispatch2 *target) {
        AlphaMovieFrame_guess *sample;
        {
            tTJSCSH lock(QueueLock);
            if(Ready.empty())
                return DisplayedFrame;
            sample = Ready.front();
            Ready.pop_front();
        }
        DisplayedFrame = sample->Frame;
        tTJSNI_Layer *layer;
        target->NativeInstanceSupport(TJS_NIS_GETINSTANCE,
                                      tTJSNC_Layer::ClassID,
                                      reinterpret_cast<iTJSNativeInstance **>(
                                          &layer));
        if(layer) {
            layer->SetSize(sample->Width, sample->Height);
            if(sample->Width > 0 && sample->Height > 0)
                layer->GetMainImage()->Update(sample->Pixels,
                                              4 * sample->Pitch, 0, 0,
                                              sample->Width, sample->Height);
            layer->SetPosition(sample->Left + Left, sample->Top + Top);
            layer->Update(false);
        }
        tTJSCSH lock(QueueLock);
        Free.push_back(sample);
        FrameNotified_guess = true;
        FreeCondition.Notify_guess();
        return DisplayedFrame;
    }

private:
    void Worker_guess() {
        while(!Exit) {
            for(;;) {
                bool empty;
                {
                    tTJSCSH lock(QueueLock);
                    empty = Free.empty();
                }
                if(!empty)
                    break;
                bool stopped;
                {
                    tTJSCSH lock(StopLock);
                    stopped = Stop;
                }
                if(stopped)
                    return;
                FreeCondition.Wait_guess();
            }
            bool stopped;
            {
                tTJSCSH lock(StopLock);
                stopped = Stop;
            }
            if(stopped)
                return;
            {
                tTJSCSH lock(QueueLock);
                auto *sample = Free.front();
                Free.pop_front();
                int decoded = Decoder.DecodeNext_guess(sample);
                if(decoded != 0) {
                    if(decoded != 2)
                        TVPThrowExceptionMessage(TJS_W("Decode error."));
                    return;
                }
                Ready.push_back(sample);
                ReadyCondition.Notify_guess();
            }
        }
        TVPOnThreadExited();
    }
};

void AlphaMovieIDCTPlatform_guess(tjs_int16 *quantization,
                                  tjs_int16 *coefficients,
                                  tjs_uint8 *pixels, int pitch) {
#if defined(__ANDROID__) && (defined(__arm__) || defined(__aarch64__)) && \
    defined(WITH_SIMD)
    JSAMPROW rows[8];
    for(int i = 0; i < 8; ++i)
        rows[i] = pixels + i * pitch;
    jsimd_idct_islow_neon(quantization, coefficients, rows, 0);
#else
    // Both iOS references forward to scalar IDCT. Wasm has no ARM NEON ABI.
    AlphaMovieIDCTScalar_guess(quantization, coefficients, pixels, pitch);
#endif
}

#undef TJS_NATIVE_CLASSID_NAME
#undef TJS_NCM_REG_THIS
#undef TJS_NATIVE_SET_ClassID
#define TJS_NCM_REG_THIS classobj
#define TJS_NATIVE_SET_ClassID TJS_NATIVE_CLASSID_NAME = TJS_NCM_CLASSID;
#define TJS_NATIVE_CLASSID_NAME ClassID_AlphaMovie_guess
tjs_int32 TJS_NATIVE_CLASSID_NAME = -1;

iTJSNativeInstance *CreateAlphaMovieInstance_guess() {
    return new AlphaMovieNative_guess;
}

iTJSDispatch2 *CreateAlphaMovieClass_guess() {
    auto *classobj = TJSCreateNativeClassForPlugin(
        TJS_W("AlphaMovie"), CreateAlphaMovieInstance_guess);

    TJS_BEGIN_NATIVE_MEMBERS(AlphaMovie)
    TJS_DECL_EMPTY_FINALIZE_METHOD

    TJS_BEGIN_NATIVE_CONSTRUCTOR_DECL(_this, AlphaMovieNative_guess, AlphaMovie) {
        return TJS_S_OK;
    }
    TJS_END_NATIVE_CONSTRUCTOR_DECL(AlphaMovie)

    TJS_BEGIN_NATIVE_METHOD_DECL(open) {
        TJS_GET_NATIVE_INSTANCE(_this, AlphaMovieNative_guess);
        if(numparams < 1)
            return TJS_E_BADPARAMCOUNT;
        _this->open(ttstr(*param[0]));
        if(result)
            result->Clear();
        return TJS_S_OK;
    }
    TJS_END_NATIVE_METHOD_DECL(open)

    TJS_BEGIN_NATIVE_METHOD_DECL(clear) {
        TJS_GET_NATIVE_INSTANCE(_this, AlphaMovieNative_guess);
        _this->clear();
        if(result)
            result->Clear();
        return TJS_S_OK;
    }
    TJS_END_NATIVE_METHOD_DECL(clear)

    TJS_BEGIN_NATIVE_METHOD_DECL(showNextImage) {
        TJS_GET_NATIVE_INSTANCE(_this, AlphaMovieNative_guess);
        if(numparams < 1)
            return TJS_E_BADPARAMCOUNT;
        tjs_int32 frame = _this->showNextImage(param[0]->AsObjectNoAddRef());
        if(result)
            *result = frame;
        return TJS_S_OK;
    }
    TJS_END_NATIVE_METHOD_DECL(showNextImage)

    TJS_BEGIN_NATIVE_METHOD_DECL(isPlaying) {
        TJS_GET_NATIVE_INSTANCE(_this, AlphaMovieNative_guess);
        bool playing = _this->isPlaying();
        if(result)
            *result = playing;
        return TJS_S_OK;
    }
    TJS_END_NATIVE_METHOD_DECL(isPlaying)

    TJS_BEGIN_NATIVE_METHOD_DECL(play) {
        TJS_GET_NATIVE_INSTANCE(_this, AlphaMovieNative_guess);
        _this->play();
        if(result)
            result->Clear();
        return TJS_S_OK;
    }
    TJS_END_NATIVE_METHOD_DECL(play)

    TJS_BEGIN_NATIVE_METHOD_DECL(stop) {
        TJS_GET_NATIVE_INSTANCE(_this, AlphaMovieNative_guess);
        _this->stop();
        if(result)
            result->Clear();
        return TJS_S_OK;
    }
    TJS_END_NATIVE_METHOD_DECL(stop)

    TJS_BEGIN_NATIVE_METHOD_DECL(setPosition) {
        TJS_GET_NATIVE_INSTANCE(_this, AlphaMovieNative_guess);
        if(numparams < 2)
            return TJS_E_BADPARAMCOUNT;
        _this->setPosition((tjs_int)*param[0], (tjs_int)*param[1]);
        if(result)
            result->Clear();
        return TJS_S_OK;
    }
    TJS_END_NATIVE_METHOD_DECL(setPosition)

    TJS_BEGIN_NATIVE_METHOD_DECL(setNextMovieFile) {
        TJS_GET_NATIVE_INSTANCE(_this, AlphaMovieNative_guess);
        if(numparams < 1)
            return TJS_E_BADPARAMCOUNT;
        _this->setNextMovieFile(ttstr(*param[0]));
        if(result)
            result->Clear();
        return TJS_S_OK;
    }
    TJS_END_NATIVE_METHOD_DECL(setNextMovieFile)

    TJS_BEGIN_NATIVE_PROP_DECL(numOfFrame) {
        TJS_BEGIN_NATIVE_PROP_GETTER {
            TJS_GET_NATIVE_INSTANCE(_this, AlphaMovieNative_guess);
            *result = static_cast<tjs_int64>(_this->Decoder.NumOfFrame);
            return TJS_S_OK;
        }
        TJS_END_NATIVE_PROP_GETTER
        TJS_DENY_NATIVE_PROP_SETTER
    }
    TJS_END_NATIVE_PROP_DECL(numOfFrame)

    TJS_BEGIN_NATIVE_PROP_DECL(frame) {
        TJS_BEGIN_NATIVE_PROP_GETTER {
            TJS_GET_NATIVE_INSTANCE(_this, AlphaMovieNative_guess);
            *result = static_cast<tjs_int64>(_this->Decoder.Frame);
            return TJS_S_OK;
        }
        TJS_END_NATIVE_PROP_GETTER
        TJS_BEGIN_NATIVE_PROP_SETTER {
            TJS_GET_NATIVE_INSTANCE(_this, AlphaMovieNative_guess);
            _this->SetFrame_guess((tjs_int)*param);
            return TJS_S_OK;
        }
        TJS_END_NATIVE_PROP_SETTER
    }
    TJS_END_NATIVE_PROP_DECL(frame)

    TJS_BEGIN_NATIVE_PROP_DECL(loop) {
        TJS_BEGIN_NATIVE_PROP_GETTER {
            TJS_GET_NATIVE_INSTANCE(_this, AlphaMovieNative_guess);
            *result = _this->Decoder.Loop;
            return TJS_S_OK;
        }
        TJS_END_NATIVE_PROP_GETTER
        TJS_BEGIN_NATIVE_PROP_SETTER {
            TJS_GET_NATIVE_INSTANCE(_this, AlphaMovieNative_guess);
            _this->Decoder.Loop = (tjs_int)*param != 0;
            return TJS_S_OK;
        }
        TJS_END_NATIVE_PROP_SETTER
    }
    TJS_END_NATIVE_PROP_DECL(loop)

    TJS_BEGIN_NATIVE_PROP_DECL(nextLoop) {
        TJS_BEGIN_NATIVE_PROP_GETTER {
            TJS_GET_NATIVE_INSTANCE(_this, AlphaMovieNative_guess);
            *result = _this->Decoder.NextLoop;
            return TJS_S_OK;
        }
        TJS_END_NATIVE_PROP_GETTER
        TJS_BEGIN_NATIVE_PROP_SETTER {
            TJS_GET_NATIVE_INSTANCE(_this, AlphaMovieNative_guess);
            _this->Decoder.NextLoop = (tjs_int)*param != 0;
            return TJS_S_OK;
        }
        TJS_END_NATIVE_PROP_SETTER
    }
    TJS_END_NATIVE_PROP_DECL(nextLoop)

    TJS_BEGIN_NATIVE_PROP_DECL(preloadSamples) {
        TJS_BEGIN_NATIVE_PROP_GETTER {
            TJS_GET_NATIVE_INSTANCE(_this, AlphaMovieNative_guess);
            *result = _this->PreloadSamples;
            return TJS_S_OK;
        }
        TJS_END_NATIVE_PROP_GETTER
        TJS_BEGIN_NATIVE_PROP_SETTER {
            TJS_GET_NATIVE_INSTANCE(_this, AlphaMovieNative_guess);
            tjs_int count = (tjs_int)*param;
            if(count >= 1 && count <= 30)
                _this->PreloadSamples = count;
            return TJS_S_OK;
        }
        TJS_END_NATIVE_PROP_SETTER
    }
    TJS_END_NATIVE_PROP_DECL(preloadSamples)

    TJS_BEGIN_NATIVE_PROP_DECL(left) {
        TJS_BEGIN_NATIVE_PROP_GETTER {
            TJS_GET_NATIVE_INSTANCE(_this, AlphaMovieNative_guess);
            *result = _this->Left;
            return TJS_S_OK;
        }
        TJS_END_NATIVE_PROP_GETTER
        TJS_BEGIN_NATIVE_PROP_SETTER {
            TJS_GET_NATIVE_INSTANCE(_this, AlphaMovieNative_guess);
            _this->Left = (tjs_int)*param;
            return TJS_S_OK;
        }
        TJS_END_NATIVE_PROP_SETTER
    }
    TJS_END_NATIVE_PROP_DECL(left)

    TJS_BEGIN_NATIVE_PROP_DECL(top) {
        TJS_BEGIN_NATIVE_PROP_GETTER {
            TJS_GET_NATIVE_INSTANCE(_this, AlphaMovieNative_guess);
            *result = _this->Top;
            return TJS_S_OK;
        }
        TJS_END_NATIVE_PROP_GETTER
        TJS_BEGIN_NATIVE_PROP_SETTER {
            TJS_GET_NATIVE_INSTANCE(_this, AlphaMovieNative_guess);
            _this->Top = (tjs_int)*param;
            return TJS_S_OK;
        }
        TJS_END_NATIVE_PROP_SETTER
    }
    TJS_END_NATIVE_PROP_DECL(top)

    TJS_BEGIN_NATIVE_PROP_DECL(screenWidth) {
        TJS_BEGIN_NATIVE_PROP_GETTER {
            TJS_GET_NATIVE_INSTANCE(_this, AlphaMovieNative_guess);
            *result = _this->Decoder.ScreenWidth;
            return TJS_S_OK;
        }
        TJS_END_NATIVE_PROP_GETTER
        TJS_DENY_NATIVE_PROP_SETTER
    }
    TJS_END_NATIVE_PROP_DECL(screenWidth)

    TJS_BEGIN_NATIVE_PROP_DECL(screenHeight) {
        TJS_BEGIN_NATIVE_PROP_GETTER {
            TJS_GET_NATIVE_INSTANCE(_this, AlphaMovieNative_guess);
            *result = _this->Decoder.ScreenHeight;
            return TJS_S_OK;
        }
        TJS_END_NATIVE_PROP_GETTER
        TJS_DENY_NATIVE_PROP_SETTER
    }
    TJS_END_NATIVE_PROP_DECL(screenHeight)

    TJS_BEGIN_NATIVE_PROP_DECL(FPSScale) {
        TJS_BEGIN_NATIVE_PROP_GETTER {
            TJS_GET_NATIVE_INSTANCE(_this, AlphaMovieNative_guess);
            *result = static_cast<tjs_int64>(_this->Decoder.FPSScale);
            return TJS_S_OK;
        }
        TJS_END_NATIVE_PROP_GETTER
        TJS_DENY_NATIVE_PROP_SETTER
    }
    TJS_END_NATIVE_PROP_DECL(FPSScale)

    TJS_BEGIN_NATIVE_PROP_DECL(FPSRate) {
        TJS_BEGIN_NATIVE_PROP_GETTER {
            TJS_GET_NATIVE_INSTANCE(_this, AlphaMovieNative_guess);
            *result = static_cast<tjs_int64>(_this->Decoder.FPSRate);
            return TJS_S_OK;
        }
        TJS_END_NATIVE_PROP_GETTER
        TJS_DENY_NATIVE_PROP_SETTER
    }
    TJS_END_NATIVE_PROP_DECL(FPSRate)

    TJS_END_NATIVE_MEMBERS
    return classobj;
}

void RegisterAlphaMovie_guess() {
    iTJSDispatch2 *global = TVPGetScriptDispatch();
    if(global) {
        {
            iTJSDispatch2 *classobj = CreateAlphaMovieClass_guess();
            tTJSVariant value(classobj);
            classobj->Release();
            global->PropSet(TJS_MEMBERENSURE, TJS_W("AlphaMovie"), nullptr,
                            &value, global);
        }
        global->Release();
    }
    if((TVPCPUType & 0x0200000f) == 0x02000003)
        AlphaMovieIDCT_guess = AlphaMovieIDCTPlatform_guess;
}

NCB_PRE_REGIST_CALLBACK(RegisterAlphaMovie_guess);

} // namespace
