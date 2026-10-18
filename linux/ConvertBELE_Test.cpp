#include <gtest/gtest.h>
#include <vector>
#include <cstdint>
#include <cstring>
#include <iostream>
#include <iomanip>

namespace OpenHome {
    namespace Media {
        size_t ConvertInterleavedToS32LE_With24Simd(
            const uint8_t* src, size_t srcBytes, uint8_t* dst, size_t framesRequested,
            unsigned channels, unsigned subsampleBytes, bool sourceLittleEndian,
            bool duplicateMono, std::vector<int32_t>& aTmpOut);
    }
}

class ConvertBELETest : public ::testing::Test {
protected:
    std::vector<int32_t> tmpBuf;
    
    void PrintResult(const std::string& testName, const std::vector<int32_t>& result) {
        std::cout << "\n" << testName << " Output:\n";
        for (size_t i = 0; i < result.size(); ++i) {
            std::cout << "  [" << i << "]: 0x" << std::hex << std::setw(8) 
                      << std::setfill('0') << (uint32_t)result[i] << std::dec << "\n";
        }
    }
};

// ============================================================================
// 1-BYTE (8-BIT PCM) TESTS
// ============================================================================
TEST_F(ConvertBELETest, S8_Conversion_BasicBoundaries) {
    // 8-bit PCM is unsigned with bias: 0 = -128, 128 = 0, 255 = +127
    std::vector<uint8_t> src = { 0, 128, 255, 64 };
    std::vector<uint8_t> dst(16, 0);

    size_t frames = OpenHome::Media::ConvertInterleavedToS32LE_With24Simd(
        src.data(), src.size(), dst.data(), 
        4, 1, 1, true, false, tmpBuf
    );

    std::vector<int32_t> result(reinterpret_cast<int32_t*>(dst.data()), 
                                reinterpret_cast<int32_t*>(dst.data()) + 4);
    PrintResult("S8_Conversion_BasicBoundaries", result);

    ASSERT_EQ(frames, 4);
    EXPECT_EQ(result[0], (-128) << 24);   // 0x80000000
    EXPECT_EQ(result[1], 0);              // 0x00000000
    EXPECT_EQ(result[2], 127 << 24);      // 0x7F000000
    EXPECT_EQ(result[3], (-64) << 24);    // 0xC0000000
}

TEST_F(ConvertBELETest, S8_Conversion_AllBytes) {
    // Extended test: verify several byte values
    std::vector<uint8_t> src = { 1, 127, 128, 129, 254 };
    std::vector<uint8_t> dst(20, 0);

    size_t frames = OpenHome::Media::ConvertInterleavedToS32LE_With24Simd(
        src.data(), src.size(), dst.data(), 5, 1, 1, true, false, tmpBuf
    );

    std::vector<int32_t> result(reinterpret_cast<int32_t*>(dst.data()), 
                                reinterpret_cast<int32_t*>(dst.data()) + 5);
    PrintResult("S8_Conversion_AllBytes", result);

    ASSERT_EQ(frames, 5);
    EXPECT_EQ(result[0], (-127) << 24);
    EXPECT_EQ(result[1], (-1) << 24);
    EXPECT_EQ(result[2], 0);
    EXPECT_EQ(result[3], 1 << 24);
    EXPECT_EQ(result[4], 126 << 24);
}

// ============================================================================
// 2-BYTE (16-BIT PCM) TESTS
// ============================================================================
TEST_F(ConvertBELETest, S16LE_Conversion_BasicBoundaries) {
    // 16-bit Little Endian: [LSB, MSB]
    std::vector<uint8_t> src = {
        0x00, 0x80,  // -32768
        0x00, 0x00,  // 0
        0xFF, 0x7F,  // 32767
        0x00, 0x01   // 256
    };
    std::vector<int32_t> dst(4, 0);

    size_t frames = OpenHome::Media::ConvertInterleavedToS32LE_With24Simd(
        src.data(), src.size(), reinterpret_cast<uint8_t*>(dst.data()), 
        4, 1, 2, true, false, tmpBuf
    );

    PrintResult("S16LE_Conversion_BasicBoundaries", dst);

    ASSERT_EQ(frames, 4);
    EXPECT_EQ(dst[0], -32768 << 16);
    EXPECT_EQ(dst[1], 0);
    EXPECT_EQ(dst[2], 32767 << 16);
    EXPECT_EQ(dst[3], 256 << 16);
}

TEST_F(ConvertBELETest, S16BE_Conversion_BasicBoundaries) {
    // 16-bit Big Endian (OpenHome): [MSB, LSB]
    std::vector<uint8_t> src = {
        0x80, 0x00,  // -32768
        0x00, 0x00,  // 0
        0x7F, 0xFF,  // 32767
        0x01, 0x00   // 256
    };
    std::vector<int32_t> dst(4, 0);

    size_t frames = OpenHome::Media::ConvertInterleavedToS32LE_With24Simd(
        src.data(), src.size(), reinterpret_cast<uint8_t*>(dst.data()), 
        4, 1, 2, false, false, tmpBuf
    );

    PrintResult("S16BE_Conversion_BasicBoundaries", dst);

    ASSERT_EQ(frames, 4);
    EXPECT_EQ(dst[0], -32768 << 16);
    EXPECT_EQ(dst[1], 0);
    EXPECT_EQ(dst[2], 32767 << 16);
    EXPECT_EQ(dst[3], 256 << 16);
}

TEST_F(ConvertBELETest, S16BE_Conversion_NegativeValues) {
    // Big Endian negative boundary tests
    std::vector<uint8_t> src = {
        0xFF, 0xFF,  // -1
        0xFF, 0x00,  // -256
        0x80, 0x00,  // -32768
        0x80, 0x01   // -32767
    };
    std::vector<int32_t> dst(4, 0);

    size_t frames = OpenHome::Media::ConvertInterleavedToS32LE_With24Simd(
        src.data(), src.size(), reinterpret_cast<uint8_t*>(dst.data()), 
        4, 1, 2, false, false, tmpBuf
    );

    PrintResult("S16BE_Conversion_NegativeValues", dst);

    ASSERT_EQ(frames, 4);
    EXPECT_EQ(dst[0], -1 << 16);
    EXPECT_EQ(dst[1], -256 << 16);
    EXPECT_EQ(dst[2], -32768 << 16);
    EXPECT_EQ(dst[3], -32767 << 16);
}

// ============================================================================
// 3-BYTE (24-BIT PCM PACKED) TESTS
// ============================================================================
TEST_F(ConvertBELETest, S24LE_Conversion_BasicBoundaries) {
    // 24-bit Little Endian: [LSB, Mid, MSB]
    std::vector<uint8_t> src = {
        0x00, 0x00, 0x80,  // -8388608 (min 24-bit)
        0x00, 0x00, 0x00,  // 0
        0xFF, 0xFF, 0x7F,  // 8388607 (max 24-bit)
        0x34, 0x12, 0x00   // 0x001234
    };
    std::vector<int32_t> dst(4, 0);

    size_t frames = OpenHome::Media::ConvertInterleavedToS32LE_With24Simd(
        src.data(), src.size(), reinterpret_cast<uint8_t*>(dst.data()), 
        4, 1, 3, true, false, tmpBuf
    );

    PrintResult("S24LE_Conversion_BasicBoundaries", dst);

    ASSERT_EQ(frames, 4);
    EXPECT_EQ(dst[0], -8388608 << 8);
    EXPECT_EQ(dst[1], 0);
    EXPECT_EQ(dst[2], 8388607 << 8);
    EXPECT_EQ(dst[3], 0x1234 << 8);
}

TEST_F(ConvertBELETest, S24BE_Conversion_BasicBoundaries) {
    // 24-bit Big Endian (OpenHome): [MSB, Mid, LSB]
    std::vector<uint8_t> src = {
        0x80, 0x00, 0x00,  // -8388608 (min 24-bit)
        0x00, 0x00, 0x00,  // 0
        0x7F, 0xFF, 0xFF,  // 8388607 (max 24-bit)
        0x00, 0x12, 0x34   // 0x001234
    };
    std::vector<int32_t> dst(4, 0);

    size_t frames = OpenHome::Media::ConvertInterleavedToS32LE_With24Simd(
        src.data(), src.size(), reinterpret_cast<uint8_t*>(dst.data()), 
        4, 1, 3, false, false, tmpBuf
    );

    PrintResult("S24BE_Conversion_BasicBoundaries", dst);

    ASSERT_EQ(frames, 4);
    EXPECT_EQ(dst[0], -8388608 << 8);
    EXPECT_EQ(dst[1], 0);
    EXPECT_EQ(dst[2], 8388607 << 8);
    EXPECT_EQ(dst[3], 0x001234 << 8);
}

TEST_F(ConvertBELETest, S24BE_Conversion_ExtendedValues) {
    // Additional 24-bit big endian test vectors
    std::vector<uint8_t> src = {
        0xFF, 0xFF, 0xFF,  // -1
        0xFF, 0x00, 0x00,  // -65536
        0x80, 0x00, 0x00,  // -8388608
        0x80, 0x00, 0x01   // -8388607
    };
    std::vector<int32_t> dst(4, 0);

    size_t frames = OpenHome::Media::ConvertInterleavedToS32LE_With24Simd(
        src.data(), src.size(), reinterpret_cast<uint8_t*>(dst.data()), 
        4, 1, 3, false, false, tmpBuf
    );

    PrintResult("S24BE_Conversion_ExtendedValues", dst);

    ASSERT_EQ(frames, 4);
    EXPECT_EQ(dst[0], -1 << 8);
    EXPECT_EQ(dst[1], -65536 << 8);
    EXPECT_EQ(dst[2], -8388608 << 8);
    EXPECT_EQ(dst[3], -8388607 << 8);
}

// ============================================================================
// 4-BYTE (32-BIT PCM) TESTS
// ============================================================================
TEST_F(ConvertBELETest, S32BE_Conversion_BasicBoundaries) {
    // 32-bit Big Endian: [MSB, ..., LSB]
    std::vector<uint8_t> src = {
        0x80, 0x00, 0x00, 0x00,  // -2147483648
        0x00, 0x00, 0x00, 0x00,  // 0
        0x7F, 0xFF, 0xFF, 0xFF,  // 2147483647
        0x00, 0x01, 0x02, 0x03   // 0x00010203
    };
    std::vector<int32_t> dst(4, 0);

    size_t frames = OpenHome::Media::ConvertInterleavedToS32LE_With24Simd(
        src.data(), src.size(), reinterpret_cast<uint8_t*>(dst.data()), 
        4, 1, 4, false, false, tmpBuf
    );

    PrintResult("S32BE_Conversion_BasicBoundaries", dst);

    ASSERT_EQ(frames, 4);
    EXPECT_EQ(dst[0], INT32_MIN);
    EXPECT_EQ(dst[1], 0);
    EXPECT_EQ(dst[2], INT32_MAX);
    EXPECT_EQ(dst[3], 0x00010203);
}

TEST_F(ConvertBELETest, S32BE_Conversion_NegativeValues) {
    // 32-bit big endian negative test vectors
    std::vector<uint8_t> src = {
        0xFF, 0xFF, 0xFF, 0xFF,  // -1
        0xFF, 0xFF, 0xFF, 0x00,  // -256
        0x80, 0x00, 0x00, 0x00,  // -2147483648
        0x80, 0x00, 0x00, 0x01   // -2147483647
    };
    std::vector<int32_t> dst(4, 0);

    size_t frames = OpenHome::Media::ConvertInterleavedToS32LE_With24Simd(
        src.data(), src.size(), reinterpret_cast<uint8_t*>(dst.data()), 
        4, 1, 4, false, false, tmpBuf
    );

    PrintResult("S32BE_Conversion_NegativeValues", dst);

    ASSERT_EQ(frames, 4);
    EXPECT_EQ(dst[0], -1);
    EXPECT_EQ(dst[1], -256);
    EXPECT_EQ(dst[2], -2147483648);
    EXPECT_EQ(dst[3], -2147483647);
}

// ============================================================================
// MULTICHANNEL TESTS
// ============================================================================
TEST_F(ConvertBELETest, S16BE_Stereo_Interleaved) {
    // Stereo 16-bit BE: L0, R0, L1, R1, ...
    std::vector<uint8_t> src = {
        0x00, 0x00,  // L0: 0
        0x7F, 0xFF,  // R0: 32767
        0x80, 0x00,  // L1: -32768
        0xFF, 0xFF   // R1: -1
    };
    std::vector<int32_t> dst(4, 0);

    size_t frames = OpenHome::Media::ConvertInterleavedToS32LE_With24Simd(
        src.data(), src.size(), reinterpret_cast<uint8_t*>(dst.data()), 
        2, 2, 2, false, false, tmpBuf
    );

    PrintResult("S16BE_Stereo_Interleaved", dst);

    ASSERT_EQ(frames, 2);
    EXPECT_EQ(dst[0], 0);                  // L0
    EXPECT_EQ(dst[1], 32767 << 16);        // R0
    EXPECT_EQ(dst[2], -32768 << 16);       // L1
    EXPECT_EQ(dst[3], (-1) << 16);         // R1
}

TEST_F(ConvertBELETest, S24BE_Stereo_Interleaved) {
    // Stereo 24-bit BE: L0, R0, L1, R1, ... (3 bytes each)
    std::vector<uint8_t> src = {
        0x00, 0x00, 0x00,  // L0: 0
        0x7F, 0xFF, 0xFF,  // R0: 8388607
        0x80, 0x00, 0x00,  // L1: -8388608
        0xFF, 0xFF, 0xFF   // R1: -1
    };
    std::vector<int32_t> dst(4, 0);

    size_t frames = OpenHome::Media::ConvertInterleavedToS32LE_With24Simd(
        src.data(), src.size(), reinterpret_cast<uint8_t*>(dst.data()), 
        2, 2, 3, false, false, tmpBuf
    );

    PrintResult("S24BE_Stereo_Interleaved", dst);

    ASSERT_EQ(frames, 2);
    EXPECT_EQ(dst[0], 0);                         // L0
    EXPECT_EQ(dst[1], 8388607 << 8);              // R0
    EXPECT_EQ(dst[2], -8388608 << 8);             // L1
    EXPECT_EQ(dst[3], (-1) << 8);                 // R1
}

// ============================================================================
// MONO DUPLICATION TESTS
// ============================================================================
TEST_F(ConvertBELETest, S16BE_MonoDuplication) {
    // Mono 16-bit input with duplication enabled
    std::vector<uint8_t> src = {
        0x7F, 0xFF,  // 32767
        0x80, 0x00,  // -32768
        0x00, 0x00   // 0
    };
    std::vector<int32_t> dst(6, 0);  // 3 input samples → 6 output (duplicated)

    size_t frames = OpenHome::Media::ConvertInterleavedToS32LE_With24Simd(
        src.data(), src.size(), reinterpret_cast<uint8_t*>(dst.data()), 
        3, 1, 2, false, true, tmpBuf
    );

    PrintResult("S16BE_MonoDuplication", dst);

    ASSERT_EQ(frames, 3);
    // Each input sample should be duplicated in output
    EXPECT_EQ(dst[0], 32767 << 16);
    EXPECT_EQ(dst[1], 32767 << 16);  // Duplicated
    EXPECT_EQ(dst[2], -32768 << 16);
    EXPECT_EQ(dst[3], -32768 << 16); // Duplicated
    EXPECT_EQ(dst[4], 0);
    EXPECT_EQ(dst[5], 0);             // Duplicated
}

TEST_F(ConvertBELETest, S24BE_MonoDuplication) {
    // Mono 24-bit input with duplication enabled
    std::vector<uint8_t> src = {
        0x7F, 0xFF, 0xFF,  // 8388607
        0x80, 0x00, 0x00,  // -8388608
        0x00, 0x00, 0x00   // 0
    };
    std::vector<int32_t> dst(6, 0);

    size_t frames = OpenHome::Media::ConvertInterleavedToS32LE_With24Simd(
        src.data(), src.size(), reinterpret_cast<uint8_t*>(dst.data()), 
        3, 1, 3, false, true, tmpBuf
    );

    PrintResult("S24BE_MonoDuplication", dst);

    ASSERT_EQ(frames, 3);
    EXPECT_EQ(dst[0], 8388607 << 8);
    EXPECT_EQ(dst[1], 8388607 << 8);  // Duplicated
    EXPECT_EQ(dst[2], -8388608 << 8);
    EXPECT_EQ(dst[3], -8388608 << 8); // Duplicated
    EXPECT_EQ(dst[4], 0);
    EXPECT_EQ(dst[5], 0);              // Duplicated
}

// ============================================================================
// BUFFER BOUNDARY & EDGE CASE TESTS
// ============================================================================
TEST_F(ConvertBELETest, PartialFrame_Dropped) {
    // 7 bytes = 2 complete 3-byte frames + 1 partial
    std::vector<uint8_t> src = {
        0x00, 0x00, 0x00,  // Frame 0
        0x00, 0x00, 0x00,  // Frame 1
        0xFF                // Incomplete frame (dropped)
    };
    std::vector<int32_t> dst(2, 0);

    size_t frames = OpenHome::Media::ConvertInterleavedToS32LE_With24Simd(
        src.data(), src.size(), reinterpret_cast<uint8_t*>(dst.data()), 
        10, 1, 3, true, false, tmpBuf
    );

    EXPECT_EQ(frames, 2);  // Only 2 complete frames processed
}

TEST_F(ConvertBELETest, ZeroFramesRequested) {
    std::vector<uint8_t> src = { 0x00, 0x00 };
    std::vector<int32_t> dst(1, 0);

    size_t frames = OpenHome::Media::ConvertInterleavedToS32LE_With24Simd(
        src.data(), src.size(), reinterpret_cast<uint8_t*>(dst.data()), 
        0, 1, 2, true, false, tmpBuf
    );

    EXPECT_EQ(frames, 0);
}

TEST_F(ConvertBELETest, NullSourcePointer) {
    std::vector<int32_t> dst(1, 0);

    size_t frames = OpenHome::Media::ConvertInterleavedToS32LE_With24Simd(
        nullptr, 0, reinterpret_cast<uint8_t*>(dst.data()), 
        1, 1, 2, true, false, tmpBuf
    );

    EXPECT_EQ(frames, 0);
}

TEST_F(ConvertBELETest, NullDestPointer) {
    std::vector<uint8_t> src = { 0x00, 0x00 };

    size_t frames = OpenHome::Media::ConvertInterleavedToS32LE_With24Simd(
        src.data(), src.size(), nullptr, 
        1, 1, 2, true, false, tmpBuf
    );

    EXPECT_EQ(frames, 0);
}

TEST_F(ConvertBELETest, InvalidSubsampleBytes) {
    std::vector<uint8_t> src = { 0x00 };
    std::vector<int32_t> dst(1, 0);

    // subsampleBytes = 0 (invalid)
    size_t frames = OpenHome::Media::ConvertInterleavedToS32LE_With24Simd(
        src.data(), src.size(), reinterpret_cast<uint8_t*>(dst.data()), 
        1, 1, 0, true, false, tmpBuf
    );
    EXPECT_EQ(frames, 0);

    // subsampleBytes = 5 (too large)
    frames = OpenHome::Media::ConvertInterleavedToS32LE_With24Simd(
        src.data(), src.size(), reinterpret_cast<uint8_t*>(dst.data()), 
        1, 1, 5, true, false, tmpBuf
    );
    EXPECT_EQ(frames, 0);
}

TEST_F(ConvertBELETest, ZeroChannels) {
    std::vector<uint8_t> src = { 0x00, 0x00 };
    std::vector<int32_t> dst(1, 0);

    size_t frames = OpenHome::Media::ConvertInterleavedToS32LE_With24Simd(
        src.data(), src.size(), reinterpret_cast<uint8_t*>(dst.data()), 
        1, 0, 2, true, false, tmpBuf
    );

    EXPECT_EQ(frames, 0);
}

TEST_F(ConvertBELETest, InsufficientSourceBytes) {
    // Request 10 frames @ 2 bytes each, but only provide 1 byte
    std::vector<uint8_t> src = { 0x00 };
    std::vector<int32_t> dst(10, 0);

    size_t frames = OpenHome::Media::ConvertInterleavedToS32LE_With24Simd(
        src.data(), src.size(), reinterpret_cast<uint8_t*>(dst.data()), 
        10, 1, 2, true, false, tmpBuf
    );

    EXPECT_EQ(frames, 0);  // Not enough bytes for even one frame
}

int main(int argc, char **argv) {
    ::testing::InitGoogleTest(&argc, argv);
    return RUN_ALL_TESTS();
}
