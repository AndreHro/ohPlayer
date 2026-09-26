#include "ConvertBELE.h"

#include <cstdint>
#include <cstddef>
#include <cstring>
#include <vector>
#include <algorithm>
#include <cassert>

using namespace OpenHome;

// x86 SIMD
#if defined(__AVX2__) || defined(__SSE2__) || defined(__SSSE3__)
    #include <immintrin.h>
#endif

// ARM NEON
#if defined(__ARM_NEON) || defined(__ARM_NEON__)
    #include <arm_neon.h>
#endif

// Host endianness detection
#if defined(__cpp_lib_endian) && (__cpp_lib_endian >= 201907L)
    #include <bit>
    static constexpr bool hostIsLittle = (std::endian::native == std::endian::little);
#elif defined(_WIN32) || defined(__LITTLE_ENDIAN__) || defined(__i386__) || defined(__x86_64__) || defined(__aarch64__)
    static constexpr bool hostIsLittle = true;
#else
    static constexpr bool hostIsLittle = false;
#endif

// ==========================================
// 8-BIT AUDIO TO S32LE PIPELINES
// ==========================================
static void ConvertS8ToS32_SIMD(const TUint8* src, TInt32* dst, size_t samples) {
    size_t i = 0;
#if defined(__AVX2__)
    __m256i bias = _mm256_set1_epi32(128);
    for (; i + 8 <= samples; i += 8) {
        __m128i raw = _mm_loadl_epi64(reinterpret_cast<const __m128i*>(src + i));
        __m256i v32 = _mm256_cvtepu8_epi32(raw);
        v32 = _mm256_sub_epi32(v32, bias);
        v32 = _mm256_slli_epi32(v32, 24);
        _mm256_storeu_si256(reinterpret_cast<__m256i*>(dst + i), v32);
    }
#elif defined(__ARM_NEON) || defined(__ARM_NEON__)
    int8x8_t bias = vdup_n_s8(-128);
    for (; i + 8 <= samples; i += 8) {
        uint8x8_t raw = vld1_u8(src + i);
        int8x8_t signed8 = vadd_s8(vreinterpret_s8_u8(raw), bias);
        int16x8_t s16 = vmovl_s8(signed8);
        int32x4_t lo32 = vshlq_n_s32(vmovl_s16(vget_low_s16(s16)), 24);
        int32x4_t hi32 = vshlq_n_s32(vmovl_s16(vget_high_s16(s16)), 24);
        vst1q_s32(dst + i, lo32);
        vst1q_s32(dst + i + 4, hi32);
    }
#endif
   for (; i < samples; ++i) {
        const TInt32 sample =
            static_cast<TInt32>(src[i]) - 128;

        dst[i] = static_cast<TInt32>(
            static_cast<TUint32>(sample) << 24);
    }
}

// ==========================================
// 16-BIT AUDIO TO S32LE PIPELINES
// ==========================================
static void ConvertS16_LE_ToS32_LE_SIMD(const TInt16* src, TInt32* dst, size_t samples) {
    size_t i = 0;
#if defined(__AVX2__)
    for (; i + 16 <= samples; i += 16) {
        __m256i in256 = _mm256_loadu_si256(reinterpret_cast<const __m256i*>(src + i));
        __m256i low32  = _mm256_cvtepi16_epi32(_mm256_castsi256_si128(in256));
        __m256i high32 = _mm256_cvtepi16_epi32(_mm256_extracti128_si256(in256, 1));
        _mm256_storeu_si256(reinterpret_cast<__m256i*>(dst + i), _mm256_slli_epi32(low32, 16));
        _mm256_storeu_si256(reinterpret_cast<__m256i*>(dst + i + 8), _mm256_slli_epi32(high32, 16));
    }
#elif defined(__ARM_NEON) || defined(__ARM_NEON__)
    for (; i + 8 <= samples; i += 8) {
        int16x8_t v16 = vld1q_s16(src + i);
        int32x4_t lo32 = vshlq_n_s32(vmovl_s16(vget_low_s16(v16)), 16);
        int32x4_t hi32 = vshlq_n_s32(vmovl_s16(vget_high_s16(v16)), 16);
        vst1q_s32(dst + i, lo32);
        vst1q_s32(dst + i + 4, hi32);
    }
#endif
    for (; i < samples; ++i) {
        const TInt32 sample = static_cast<TInt32>(src[i]);

        dst[i] = static_cast<TInt32>(
            static_cast<TUint32>(sample) << 16);
    }
}
static void ConvertS16_BE_ToS32_LE_SIMD(const TInt16* src, TInt32* dst, size_t samples) {
    size_t i = 0;
    
#if defined(__AVX2__)
    const __m256i swapBytesMask = _mm256_setr_epi8(
        1,0, 3,2, 5,4, 7,6, 9,8, 11,10, 13,12, 15,14,
        1,0, 3,2, 5,4, 7,6, 9,8, 11,10, 13,12, 15,14
    );
    for (; i + 16 <= samples; i += 16) {
        __m256i in256 = _mm256_loadu_si256(reinterpret_cast<const __m256i*>(src + i));
        in256 = _mm256_shuffle_epi8(in256, swapBytesMask);
        __m256i low32  = _mm256_cvtepi16_epi32(_mm256_castsi256_si128(in256));
        __m256i high32 = _mm256_cvtepi16_epi32(_mm256_extracti128_si256(in256, 1));
        _mm256_storeu_si256(reinterpret_cast<__m256i*>(dst + i), _mm256_slli_epi32(low32, 16));
        _mm256_storeu_si256(reinterpret_cast<__m256i*>(dst + i + 8), _mm256_slli_epi32(high32, 16));
    }
#elif defined(__ARM_NEON) || defined(__ARM_NEON__)
    for (; i + 8 <= samples; i += 8) {
        uint8x16_t raw_be = vld1q_u8(reinterpret_cast<const TUint8*>(src + i));
        int16x8_t v16 = vreinterpretq_s16_u8(vrev16q_u8(raw_be));
        int32x4_t lo32 = vshlq_n_s32(vmovl_s16(vget_low_s16(v16)), 16);
        int32x4_t hi32 = vshlq_n_s32(vmovl_s16(vget_high_s16(v16)), 16);
        vst1q_s32(dst + i, lo32);
        vst1q_s32(dst + i + 4, hi32);
    }
#endif
    for (; i < samples; ++i) {
        const TUint8* b =
            reinterpret_cast<const TUint8*>(src) + (i * 2);

        const TUint16 raw =
            (static_cast<TUint16>(b[0]) << 8) |
            static_cast<TUint16>(b[1]);

        const TInt32 sample =
            (raw & 0x8000u)
                ? static_cast<TInt32>(raw | 0xffff0000u)
                : static_cast<TInt32>(raw);

        dst[i] = static_cast<TInt32>(
            static_cast<TUint32>(sample) << 16);
    }
}

// ==========================================
// 24-BIT AUDIO TO S32LE PIPELINES (OPENHOME MATCH)
// ==========================================
static void ConvertS24_LE_ToS32_LE_SIMD(const TUint8* src, TInt32* dst, size_t samples) {
    size_t i = 0;
#if defined(__AVX2__) && defined(__SSSE3__)
    // Maps [0, 1, 2] bytes to standard 32-bit Little Endian layout slot base
    const __m128i shuffleMaskLE = _mm_setr_epi8(
        0, 1, 2, (char)0x80,
        3, 4, 5, (char)0x80,
        6, 7, 8, (char)0x80,
        9, 10, 11, (char)0x80);

    // Terminate 2 elements early (24 bytes) to safely allow unaligned 16-byte vector loads
    for (; i + 10 <= samples; i += 8) {
        const TUint8* base = src + i * 3;

        // Fast path direct unaligned vector loads (zero heap or stack allocation copies)
        __m128i first  = _mm_loadu_si128(reinterpret_cast<const __m128i*>(base));
        __m128i second = _mm_loadu_si128(reinterpret_cast<const __m128i*>(base + 12));

        __m128i out0 = _mm_shuffle_epi8(first, shuffleMaskLE);
        __m128i out1 = _mm_shuffle_epi8(second, shuffleMaskLE);

        __m256i combined = _mm256_setr_m128i(out0, out1);

        // Fixed: Shifts 32-bit values left by 8 bits to align with the sign bit boundary
        combined = _mm256_slli_epi32(combined, 8);

        _mm256_storeu_si256(reinterpret_cast<__m256i*>(dst + i), combined);
    }
#elif defined(__ARM_NEON) || defined(__ARM_NEON__)
    for (; i + 8 <= samples; i += 8) {
        uint8x8x3_t v = vld3_u8(src + (i * 3)); 
        uint16x8_t lo0 = vmovl_u8(v.val[0]), 
                   lo1 = vmovl_u8(v.val[1]),
                   lo2 = vmovl_u8(v.val[2]);
        
        uint32x4_t lo_words = vorrq_u32(vmovl_u16(vget_low_u16(lo0)), 
                                        vshlq_n_u32(vmovl_u16(vget_low_u16(lo1)), 8));
        lo_words = vorrq_u32(lo_words, vshlq_n_u32(vmovl_u16(vget_low_u16(lo2)), 16));
        vst1q_s32(dst + i, vreinterpretq_s32_u32(vshlq_n_u32(lo_words, 8)));

        uint32x4_t hi_words = vorrq_u32(vmovl_u16(vget_high_u16(lo0)), 
                                        vshlq_n_u32(vmovl_u16(vget_high_u16(lo1)), 8));
        hi_words = vorrq_u32(hi_words, vshlq_n_u32(vmovl_u16(vget_high_u16(lo2)), 16));
        vst1q_s32(dst + i + 4, vreinterpretq_s32_u32(vshlq_n_u32(hi_words, 8)));
    }
#endif

    // Clean scalar execution for loop tail elements
    for (; i < samples; ++i) {
        TUint32 v = src[i*3] | (src[i*3+1] << 8) | (src[i*3+2] << 16);
        dst[i] = static_cast<TInt32>(v << 8);
    }
}

static void ConvertS24_BE_ToS32_LE_SIMD(const TUint8* src, TInt32* dst, size_t samples) {
    size_t i = 0;
#if defined(__AVX2__) && defined(__SSSE3__)
    // BE-to-LE Shuffle Mask:
    // BE input arrives as [MSB, Mid, LSB] per sample.
    // We need to reorder to [LSB, Mid, MSB, 0x80] so that the subsequent
    // left shift by 8 produces the correct left-justified S32_LE value.
    const __m128i shuffleMaskBE = _mm_setr_epi8(
        2, 1, 0, (char)0x80,
        5, 4, 3, (char)0x80,
        8, 7, 6, (char)0x80,
        11, 10, 9, (char)0x80);

    for (; i + 10 <= samples; i += 8) {
        const TUint8* base = src + i * 3;

        __m128i first  = _mm_loadu_si128(reinterpret_cast<const __m128i*>(base));
        __m128i second = _mm_loadu_si128(reinterpret_cast<const __m128i*>(base + 12));

        __m128i out0 = _mm_shuffle_epi8(first, shuffleMaskBE);
        __m128i out1 = _mm_shuffle_epi8(second, shuffleMaskBE);

        __m256i combined = _mm256_setr_m128i(out0, out1);
        
        // Shift left to properly sign-extend from the 24-bit boundary to 32-bit
        combined = _mm256_slli_epi32(combined, 8);

        _mm256_storeu_si256(reinterpret_cast<__m256i*>(dst + i), combined);
    }
#elif defined(__ARM_NEON) || defined(__ARM_NEON__)
    for (; i + 8 <= samples; i += 8) {
        uint8x8x3_t v = vld3_u8(src + (i * 3)); 
        
        // OpenHome network layout assigns:
        // v.val[0] = MSB, v.val[1] = Mid, v.val[2] = LSB
        uint16x8_t lo_msb = vmovl_u8(v.val[0]);
        uint16x8_t lo_mid = vmovl_u8(v.val[1]);
        uint16x8_t lo_lsb = vmovl_u8(v.val[2]);
        
        // Reconstruct words as Little-Endian: LSB | (Mid << 8) | (MSB << 16)
        uint32x4_t lo_words = vorrq_u32(vmovl_u16(vget_low_u16(lo_lsb)), vshlq_n_u32(vmovl_u16(vget_low_u16(lo_mid)), 8));
        lo_words = vorrq_u32(lo_words, vshlq_n_u32(vmovl_u16(vget_low_u16(lo_msb)), 16));
        vst1q_s32(dst + i, vreinterpretq_s32_u32(vshlq_n_u32(lo_words, 8)));

        uint32x4_t hi_words = vorrq_u32(vmovl_u16(vget_high_u16(lo_lsb)), vshlq_n_u32(vmovl_u16(vget_high_u16(lo_mid)), 8));
        hi_words = vorrq_u32(hi_words, vshlq_n_u32(vmovl_u16(vget_high_u16(lo_msb)), 16));
        vst1q_s32(dst + i + 4, vreinterpretq_s32_u32(vshlq_n_u32(hi_words, 8)));
    }
#endif

    // Correct scalar fallback for network streams
    for (; i < samples; ++i) {
        TUint32 v = (static_cast<TUint32>(src[i*3]) << 16) | 
                     (static_cast<TUint32>(src[i*3+1]) << 8) | 
                      static_cast<TUint32>(src[i*3+2]);
        dst[i] = static_cast<TInt32>(v << 8);
    }
}

// ==========================================
// 32-BIT AUDIO TO S32LE PIPELINES
// ==========================================
static void ConvertS32_BE_ToS32_LE_SIMD(const TInt32* src, TInt32* dst, size_t samples) {
    size_t i = 0;
#if defined(__AVX2__)
    const __m256i swap32Mask = _mm256_setr_epi8(
        3,2,1,0, 7,6,5,4, 11,10,9,8, 15,14,13,12, 
        3,2,1,0, 7,6,5,4, 11,10,9,8, 15,14,13,12
    );
    for (; i + 8 <= samples; i += 8) {
        _mm256_storeu_si256(
            reinterpret_cast<__m256i*>(dst + i), 
            _mm256_shuffle_epi8(_mm256_loadu_si256(reinterpret_cast<const __m256i*>(src + i)), swap32Mask)
        );
    }
#elif defined(__ARM_NEON) || defined(__ARM_NEON__)
    for (; i + 4 <= samples; i += 4) {
        uint8x16_t raw_be = vld1q_u8(reinterpret_cast<const TUint8*>(src + i));
        vst1q_s32(dst + i, vreinterpretq_s32_u8(vrev32q_u8(raw_be)));
    }
#endif

    // Corrected scalar fallback for loop tail elements
    for (; i < samples; ++i) {
        const TUint8* s = reinterpret_cast<const TUint8*>(src + i);
        // Explicitly promote to uTInt32 prior to shifting to prevent signed bit overflows
        TUint32 v = (static_cast<TUint32>(s[0]) << 24) |
                     (static_cast<TUint32>(s[1]) << 16) |
                     (static_cast<TUint32>(s[2]) << 8)  |
                      static_cast<TUint32>(s[3]);
        dst[i] = static_cast<TInt32>(v);
    }
}

static inline void WriteS32LE(TUint8*& dst, TInt32 sample)
{
    const TUint32 value = static_cast<TUint32>(sample);

    dst[0] = static_cast<TUint8>(value);
    dst[1] = static_cast<TUint8>(value >> 8);
    dst[2] = static_cast<TUint8>(value >> 16);
    dst[3] = static_cast<TUint8>(value >> 24);
    dst += 4;
}

// ==========================================
// TOP LEVEL INTERLEAVED ENTRY POINT
// ==========================================
size_t OpenHome::Media::ConvertInterleavedToS32LE_With24Simd(
    const TByte* src,
    size_t srcBytes,
    TUint8* dst,
    size_t framesRequested,
    unsigned channels,
    unsigned subsampleBytes,
    bool sourceLittleEndian,
    bool duplicateMono,
    std::vector<TInt32>& aTmpOut)
{
    if (src == nullptr ||
        dst == nullptr ||
        framesRequested == 0 ||
        channels == 0 ||
        subsampleBytes < 1 ||
        subsampleBytes > 4) {
        return 0;
    }

    if (channels > SIZE_MAX / subsampleBytes) {
        return 0;
    }

    const size_t bytesPerFrame =
        static_cast<size_t>(channels) * subsampleBytes;

    const size_t frames =
        std::min(static_cast<size_t>(framesRequested),
                 srcBytes / bytesPerFrame);

    if (frames == 0 ||
        frames > SIZE_MAX / channels) {
        return 0;
    }

    const size_t totalSamples =
        frames * static_cast<size_t>(channels);

    if (aTmpOut.size() < totalSamples) {
        aTmpOut.resize(totalSamples);
    }

    // ----------------------------------------------------
    // SUB-SAMPLE DISPATCH ROUTINES
    // ----------------------------------------------------
    if (subsampleBytes == 1) {
        ConvertS8ToS32_SIMD(src, aTmpOut.data(), totalSamples);
    }
    else if (subsampleBytes == 2) {
        const TInt16* src16 = reinterpret_cast<const TInt16*>(src);
        if (sourceLittleEndian == hostIsLittle) {
            ConvertS16_LE_ToS32_LE_SIMD(src16, aTmpOut.data(), totalSamples);
        } else {
            ConvertS16_BE_ToS32_LE_SIMD(src16, aTmpOut.data(), totalSamples);
        }
    }
    else if (subsampleBytes == 3) {
        if (sourceLittleEndian == hostIsLittle) {
            ConvertS24_LE_ToS32_LE_SIMD(src, aTmpOut.data(), totalSamples);
        } else {
            ConvertS24_BE_ToS32_LE_SIMD(src, aTmpOut.data(), totalSamples);
        }
    }
    else if (subsampleBytes == 4) {
        const TInt32* src32 = reinterpret_cast<const TInt32*>(src);
        if (sourceLittleEndian == hostIsLittle) {
            std::memcpy(aTmpOut.data(), src32, totalSamples * sizeof(TInt32));
        } else {
            ConvertS32_BE_ToS32_LE_SIMD(src32, aTmpOut.data(), totalSamples);
        }
    }

    TUint8* outPtr = dst;
    if (hostIsLittle && !(duplicateMono && channels == 1)) {
        std::memcpy(
            dst,
            aTmpOut.data(),
            totalSamples * sizeof(TInt32));
    }
    else {
        TUint8* outPtr = dst;

        for (size_t i = 0; i < totalSamples; ++i) {
            WriteS32LE(outPtr, aTmpOut[i]);

            if (duplicateMono && channels == 1) {
                WriteS32LE(outPtr, aTmpOut[i]);
            }
        }
    }

    return frames;
}
