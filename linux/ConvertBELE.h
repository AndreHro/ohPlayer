#pragma once

#include <OpenHome/OhNetTypes.h>
#include <cstddef>
#include <vector>

namespace OpenHome
{
namespace Media
{
    size_t ConvertInterleavedToS32LE_With24Simd(
        const TByte* src, size_t srcBytes, TUint8* dst,
        size_t framesRequested, unsigned channels, unsigned subsampleBytes,
        bool sourceLittleEndian, bool duplicateMono,
        std::vector<TInt32>& aTmpOut);
}
};