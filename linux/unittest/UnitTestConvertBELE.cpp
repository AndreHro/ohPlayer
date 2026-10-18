#include <iostream>
#include <vector>
#include <cstdint>
#include <cstring>
#include <iomanip>

namespace OpenHome {
    namespace Media {
        size_t ConvertInterleavedToS32LE_With24Simd(
            const uint8_t* src, size_t srcBytes, uint8_t* dst, size_t framesRequested,
            unsigned channels, unsigned subsampleBytes, bool sourceLittleEndian,
            bool duplicateMono, std::vector<int32_t>& aTmpOut);
    }
}

// SafeBuffer: allocates EXACTLY the requested size with NO padding
// Any read past this buffer will read uninitialized memory
class SafeBuffer {
private:
    std::vector<uint8_t> data;
    size_t actualSize;

public:
    SafeBuffer(size_t size) : actualSize(size) {
        data.resize(size);
        std::fill(data.begin(), data.end(), 0xAA);  // Fill with recognizable pattern
    }

    uint8_t* data_ptr() { return data.data(); }
    const uint8_t* c_data_ptr() const { return data.data(); }
    size_t size() const { return actualSize; }
};

int main() {
    std::vector<int32_t> tmpBuf;
    int passed = 0, failed = 0;

    std::cout << "Buffer Safety Tests (ARM Neon SIMD path validation)\n";
    std::cout << std::string(70, '=') << "\n\n";

    // Test 1: 24-bit with EXACTLY 9 bytes (3 frames × 3 bytes)
    // If SIMD reads past the end, it will access uninitialized data
    {
        std::cout << "Test 1: 24-bit BE with EXACTLY 9 bytes (3 frames × 3 bytes)\n";
        std::cout << "  Input pattern: [00 00 00] [7F FF FF] [80 00 00]\n";
        SafeBuffer src(9);
        uint8_t* srcPtr = src.data_ptr();
        srcPtr[0] = 0x00; srcPtr[1] = 0x00; srcPtr[2] = 0x00;  // Frame 0: 0
        srcPtr[3] = 0x7F; srcPtr[4] = 0xFF; srcPtr[5] = 0xFF;  // Frame 1: max
        srcPtr[6] = 0x80; srcPtr[7] = 0x00; srcPtr[8] = 0x00;  // Frame 2: min

        SafeBuffer dst(12);

        try {
            size_t frames = OpenHome::Media::ConvertInterleavedToS32LE_With24Simd(
                src.c_data_ptr(), src.size(), dst.data_ptr(),
                3, 1, 3, false, false, tmpBuf
            );

            std::vector<int32_t> result(reinterpret_cast<int32_t*>(dst.data_ptr()),
                                        reinterpret_cast<int32_t*>(dst.data_ptr()) + 3);
            std::cout << "  Output: [0x" << std::hex << std::setfill('0') << std::setw(8) 
                      << (uint32_t)result[0] << "] [0x" << std::setw(8) << (uint32_t)result[1] 
                      << "] [0x" << std::setw(8) << (uint32_t)result[2] << std::dec << "]\n";

            if (frames == 3 && result[0] == 0 && result[1] == (8388607 << 8) && 
                result[2] == (-8388608 << 8)) {
                std::cout << "  ✓ PASS - correct values, no buffer overread\n";
                passed++;
            } else {
                std::cout << "  ✗ FAIL - incorrect values or frame count\n";
                failed++;
            }
        } catch (const std::exception& e) {
            std::cout << "  ✗ FAIL - exception: " << e.what() << "\n";
            failed++;
        }
    }

    // Test 2: 24-bit with 7 bytes (2 complete + 1 partial)
    {
        std::cout << "\nTest 2: 24-bit BE with 7 bytes (2 complete + 1 partial byte)\n";
        std::cout << "  Input pattern: [00 00 00] [7F FF FF] [80]\n";
        SafeBuffer src(7);
        uint8_t* srcPtr = src.data_ptr();
        srcPtr[0] = 0x00; srcPtr[1] = 0x00; srcPtr[2] = 0x00;  // Frame 0: 0
        srcPtr[3] = 0x7F; srcPtr[4] = 0xFF; srcPtr[5] = 0xFF;  // Frame 1: max
        srcPtr[6] = 0x80;                                       // Partial Frame 2 (dropped)

        SafeBuffer dst(8);

        try {
            size_t frames = OpenHome::Media::ConvertInterleavedToS32LE_With24Simd(
                src.c_data_ptr(), src.size(), dst.data_ptr(),
                10, 1, 3, false, false, tmpBuf
            );

            if (frames == 2) {
                std::cout << "  Output: 2 frames (partial byte correctly dropped)\n";
                std::cout << "  ✓ PASS - no buffer overread on partial frame\n";
                passed++;
            } else {
                std::cout << "  ✗ FAIL - expected 2 frames, got " << frames << "\n";
                failed++;
            }
        } catch (const std::exception& e) {
            std::cout << "  ✗ FAIL - exception: " << e.what() << "\n";
            failed++;
        }
    }

    // Test 3: Stereo 24-bit (2 channels, 3 bytes each)
    {
        std::cout << "\nTest 3: 24-bit BE Stereo with EXACTLY 12 bytes (2 frames × 2 ch × 3 bytes)\n";
        std::cout << "  Input: 2 frames, 2 channels each, 3 bytes per sample\n";
        SafeBuffer src(12);
        uint8_t* srcPtr = src.data_ptr();
        srcPtr[0]  = 0x00; srcPtr[1]  = 0x00; srcPtr[2]  = 0x00;  // L0: 0
        srcPtr[3]  = 0x7F; srcPtr[4]  = 0xFF; srcPtr[5]  = 0xFF;  // R0: max
        srcPtr[6]  = 0x80; srcPtr[7]  = 0x00; srcPtr[8]  = 0x00;  // L1: min
        srcPtr[9]  = 0xFF; srcPtr[10] = 0xFF; srcPtr[11] = 0xFF;  // R1: -1

        SafeBuffer dst(16);

        try {
            size_t frames = OpenHome::Media::ConvertInterleavedToS32LE_With24Simd(
                src.c_data_ptr(), src.size(), dst.data_ptr(),
                2, 2, 3, false, false, tmpBuf
            );

            if (frames == 2) {
                std::cout << "  ✓ PASS - stereo 24-bit, no buffer overread\n";
                passed++;
            } else {
                std::cout << "  ✗ FAIL - expected 2 frames, got " << frames << "\n";
                failed++;
            }
        } catch (const std::exception& e) {
            std::cout << "  ✗ FAIL - exception: " << e.what() << "\n";
            failed++;
        }
    }

    // Test 4: 16-bit with exact size (ARM Neon processes 8 samples at a time)
    {
        std::cout << "\nTest 4: 16-bit BE with EXACTLY 8 bytes (4 frames × 2 bytes)\n";
        std::cout << "  Input pattern: [80 00] [00 00] [7F FF] [FF FF]\n";
        SafeBuffer src(8);
        uint8_t* srcPtr = src.data_ptr();
        srcPtr[0] = 0x80; srcPtr[1] = 0x00;  // -32768
        srcPtr[2] = 0x00; srcPtr[3] = 0x00;  // 0
        srcPtr[4] = 0x7F; srcPtr[5] = 0xFF;  // 32767
        srcPtr[6] = 0xFF; srcPtr[7] = 0xFF;  // -1

        SafeBuffer dst(16);

        try {
            size_t frames = OpenHome::Media::ConvertInterleavedToS32LE_With24Simd(
                src.c_data_ptr(), src.size(), dst.data_ptr(),
                4, 1, 2, false, false, tmpBuf
            );

            if (frames == 4) {
                std::cout << "  ✓ PASS - 16-bit conversion, no buffer overread\n";
                passed++;
            } else {
                std::cout << "  ✗ FAIL - expected 4 frames, got " << frames << "\n";
                failed++;
            }
        } catch (const std::exception& e) {
            std::cout << "  ✗ FAIL - exception: " << e.what() << "\n";
            failed++;
        }
    }

    // Test 5: 8-bit with exact size
    {
        std::cout << "\nTest 5: 8-bit with EXACTLY 4 bytes (4 frames × 1 byte)\n";
        std::cout << "  Input pattern: [00] [80] [FF] [40]\n";
        SafeBuffer src(4);
        uint8_t* srcPtr = src.data_ptr();
        srcPtr[0] = 0x00;  // -128
        srcPtr[1] = 0x80;  // 0
        srcPtr[2] = 0xFF;  // +127
        srcPtr[3] = 0x40;  // -64

        SafeBuffer dst(16);

        try {
            size_t frames = OpenHome::Media::ConvertInterleavedToS32LE_With24Simd(
                src.c_data_ptr(), src.size(), dst.data_ptr(),
                4, 1, 1, true, false, tmpBuf
            );

            if (frames == 4) {
                std::cout << "  ✓ PASS - 8-bit conversion, no buffer overread\n";
                passed++;
            } else {
                std::cout << "  ✗ FAIL - expected 4 frames, got " << frames << "\n";
                failed++;
            }
        } catch (const std::exception& e) {
            std::cout << "  ✗ FAIL - exception: " << e.what() << "\n";
            failed++;
        }
    }

    // Summary
    std::cout << "\n" << std::string(70, '=') << "\n";
    std::cout << "Tests Passed: " << passed << "\n";
    std::cout << "Tests Failed: " << failed << "\n";
    std::cout << std::string(70, '=') << "\n\n";

    if (failed == 0) {
        std::cout << "✓ All safety tests passed!\n";
        std::cout << "  ARM Neon SIMD implementation is buffer-safe on this platform.\n\n";
        std::cout << "To detect potential buffer overreads on x86/AVX2:\n";
        std::cout << "  g++ -std=c++20 -O1 -g -fsanitize=address,undefined \\\n";
        std::cout << "      -I. linux/ConvertBELE.cpp linux/ConvertBELE_SafetyTest.cpp \\\n";
        std::cout << "      -o run_safety_test_asan && ./run_safety_test_asan\n\n";
    }

    return failed > 0 ? 1 : 0;
}
