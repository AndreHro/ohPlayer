#include "KfrPcmProcessor.h"

#include "IDataSink.h"
#include <OpenHome/Media/Pipeline/Msg.h>
#include <OpenHome/Private/Printer.h>

#include <kfr/base.hpp>
#include <kfr/dft.hpp>
#include <kfr/dsp.hpp>

#include <algorithm>
#include <bit>
#include <cmath>
#include <cstdint>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <limits>
#include <map>
#include <memory>
#include <mutex>
#include <numeric>
#include <string>
#include <vector>

using namespace OpenHome;
using namespace OpenHome::Media;

namespace {

constexpr TUint kOutputSampleBytes = sizeof(TInt32);
constexpr size_t kDefaultConvolveBlockSize = 1024;
constexpr size_t kLowLatencyConvolveBlockSize = 256;

int64_t SignExtend(uint32_t aValue, unsigned aBits)
{
    const uint32_t signBit = uint32_t{1} << (aBits - 1);
    return (aValue & signBit)
        ? static_cast<int64_t>(aValue) - (int64_t{1} << aBits)
        : static_cast<int64_t>(aValue);
}

struct LoadedIr
{
    kfr::univector<double> taps;
    uint32_t sampleRate{0};
    size_t peakDelaySamples{0};
};

// Minimal RIFF/WAV parser that reads fmt sample rate and 16/24/32-bit PCM or 32-bit float mono data
bool LoadMonoWavIr(const std::filesystem::path& aPath, LoadedIr& aOut)
{
    std::ifstream file(aPath, std::ios::binary);
    if (!file.is_open()) {
        return false;
    }

    char riff[4];
    uint32_t riffSize = 0;
    char wave[4];
    file.read(riff, 4);
    file.read(reinterpret_cast<char*>(&riffSize), 4);
    file.read(wave, 4);
    if (!file || std::memcmp(riff, "RIFF", 4) != 0 || std::memcmp(wave, "WAVE", 4) != 0) {
        return false;
    }

    uint16_t audioFormat = 0;
    uint16_t numChannels = 0;
    uint32_t sampleRate = 0;
    uint16_t bitsPerSample = 0;
    bool hasFmt = false;

    while (file) {
        char chunkId[4];
        uint32_t chunkSize = 0;
        file.read(chunkId, 4);
        file.read(reinterpret_cast<char*>(&chunkSize), 4);
        if (!file) {
            break;
        }

        if (std::memcmp(chunkId, "fmt ", 4) == 0 && chunkSize >= 16) {
            uint32_t byteRate = 0;
            uint16_t blockAlign = 0;
            file.read(reinterpret_cast<char*>(&audioFormat), 2);
            file.read(reinterpret_cast<char*>(&numChannels), 2);
            file.read(reinterpret_cast<char*>(&sampleRate), 4);
            file.read(reinterpret_cast<char*>(&byteRate), 4);
            file.read(reinterpret_cast<char*>(&blockAlign), 2);
            file.read(reinterpret_cast<char*>(&bitsPerSample), 2);
            if (audioFormat == 0xFFFE && chunkSize >= 40) {
                uint8_t extension[24];
                file.read(reinterpret_cast<char*>(extension), sizeof(extension));
                if (!file) {
                    return false;
                }
                audioFormat = static_cast<uint16_t>(extension[8] |
                    (static_cast<uint16_t>(extension[9]) << 8));
                if (chunkSize > 40) {
                    file.seekg(chunkSize - 40, std::ios::cur);
                }
            }
            else if (chunkSize > 16) {
                file.seekg(chunkSize - 16, std::ios::cur);
            }
            if (chunkSize & 1) {
                file.seekg(1, std::ios::cur);
            }
            if (!file) {
                return false;
            }
            hasFmt = true;
        }
        else if (std::memcmp(chunkId, "data", 4) == 0 && hasFmt && numChannels >= 1 && sampleRate > 0) {
            const uint32_t bytesPerSample = bitsPerSample / 8;
            if (bitsPerSample % 8 != 0 || bytesPerSample < 2 || bytesPerSample > 4) {
                return false;
            }
            const uint32_t frameBytes = bytesPerSample * numChannels;

            // Clamp the declared size to what is really left in the file
            // (corrupt headers, streamed WAVs with 0xFFFFFFFF)
            const std::streamoff dataStart = file.tellg();
            file.seekg(0, std::ios::end);
            const std::streamoff fileEnd = file.tellg();
            file.seekg(dataStart);
            if (!file || fileEnd < dataStart) {
                return false;
            }
            const uint64_t available = static_cast<uint64_t>(fileEnd - dataStart);
            const uint64_t dataBytes = std::min<uint64_t>(chunkSize, available);
            const size_t totalFrames = static_cast<size_t>(dataBytes / frameBytes);
            if (totalFrames == 0) {
                return false;
            }

            aOut.taps.resize(totalFrames);
            aOut.sampleRate = sampleRate;

            // Decode in blocks instead of reading the whole chunk into a second buffer
            constexpr size_t kBlockFrames = 16384;
            std::vector<uint8_t> block(kBlockFrames * frameBytes);
            double maxAbs = -1.0;
            size_t peakIdx = 0;
            size_t done = 0;
            while (done < totalFrames) {
                const size_t n = std::min(kBlockFrames, totalFrames - done);
                file.read(reinterpret_cast<char*>(block.data()),
                          static_cast<std::streamsize>(n * frameBytes));
                if (!file) {
                    return false;
                }
                for (size_t i = 0; i < n; ++i) {
                    const uint8_t* p = block.data() + i * frameBytes; // Channel 0
                    double val = 0.0;
                    if (audioFormat == 3 && bitsPerSample == 32) {
                        float f = 0.0f;
                        std::memcpy(&f, p, 4);
                        val = static_cast<double>(f);
                    }
                    else if (audioFormat == 1 && bitsPerSample == 16) {
                        const uint32_t v = static_cast<uint32_t>(p[0]) |
                                           (static_cast<uint32_t>(p[1]) << 8);
                        val = static_cast<double>(SignExtend(v, 16)) / 32768.0;
                    }
                    else if (audioFormat == 1 && bitsPerSample == 24) {
                        const uint32_t v = static_cast<uint32_t>(p[0]) |
                                           (static_cast<uint32_t>(p[1]) << 8) |
                                           (static_cast<uint32_t>(p[2]) << 16);
                        val = static_cast<double>(SignExtend(v, 24)) / 8388608.0;
                    }
                    else if (audioFormat == 1 && bitsPerSample == 32) {
                        const uint32_t v = static_cast<uint32_t>(p[0]) |
                                           (static_cast<uint32_t>(p[1]) << 8) |
                                           (static_cast<uint32_t>(p[2]) << 16) |
                                           (static_cast<uint32_t>(p[3]) << 24);
                        val = static_cast<double>(SignExtend(v, 32)) / 2147483648.0;
                    }
                    else {
                        return false;
                    }

                    aOut.taps[done + i] = val;
                    const double absVal = std::abs(val);
                    if (absVal > maxAbs) {
                        maxAbs = absVal;
                        peakIdx = done + i;
                    }
                }
                done += n;
            }

            aOut.peakDelaySamples = peakIdx;
            return !aOut.taps.empty();
        }
        else {
            file.seekg(chunkSize + (chunkSize & 1), std::ios::cur);
        }
    }
    return false;
}

// One-shot resampler for IR taps when WAV sample rate != target FIR stage rate
void ResampleIrIfNeeded(LoadedIr& aIr, uint32_t aTargetRate)
{
    if (aIr.taps.empty() || aIr.sampleRate == 0 || aIr.sampleRate == aTargetRate) {
        return;
    }

    const uint32_t g = std::gcd(aTargetRate, aIr.sampleRate);
    const size_t interp = aTargetRate / g;
    const size_t decim  = aIr.sampleRate / g;

    auto conv = kfr::sample_rate_converter<double>(
        kfr::resample_quality::high, interp, decim);

    // The converter delays its output by get_delay() samples and rings out after the last
    // input sample. Ask for the whole IR plus the ring-out, feed enough zeros, then drop the
    // leading delay so the IR keeps its original timing.
    const size_t delayOut = conv.get_delay();
    const size_t irOut = (aIr.taps.size() * interp + decim - 1) / decim;
    const size_t totalOut = irOut + 2 * delayOut;
    const size_t inNeeded = static_cast<size_t>(conv.input_size_for_output(totalOut));

    kfr::univector<double> padded(std::max(inNeeded, aIr.taps.size()), 0.0);
    std::copy(aIr.taps.begin(), aIr.taps.end(), padded.begin());
    kfr::univector<double> resampled(totalOut, 0.0);
    conv.process(resampled, padded);

    // Keep the DC gain (frequency response) of the IR: the signal amplitude is preserved by
    // the converter, so the sum of the taps changes by interp/decim
    const double gainScale = static_cast<double>(decim) / static_cast<double>(interp);
    kfr::univector<double> trimmed(irOut + delayOut, 0.0);
    double maxAbs = -1.0;
    size_t peakIdx = 0;
    for (size_t i = 0; i < trimmed.size(); ++i) {
        const double v = resampled[delayOut + i] * gainScale;
        trimmed[i] = v;
        if (std::abs(v) > maxAbs) {
            maxAbs = std::abs(v);
            peakIdx = i;
        }
    }

    aIr.taps = std::move(trimmed);
    aIr.sampleRate = aTargetRate;
    aIr.peakDelaySamples = peakIdx;
}

struct IrSet
{
    LoadedIr left;
    LoadedIr right;
    bool hasLeft{false};
};

// Cache of loaded and resampled IRs so that stop/seek/track change does not hit the disk
// or the IR resampler again. Entries are keyed by directory + file timestamps (+ rate),
// so a changed IR file is picked up automatically.
std::mutex gIrCacheMutex;
std::map<std::string, std::shared_ptr<const IrSet>> gIrCache;

std::string IrStamp(const std::filesystem::path& aDir)
{
    std::string stamp = aDir.string();
    for (const char* name : {"ir_tv_left.wav", "ir_tv_right.wav", "left_ir.wav", "ir_left.wav", "right_ir.wav", "ir_right.wav"}) {
        std::error_code ec;
        const auto t = std::filesystem::last_write_time(aDir / name, ec);
        stamp += '|';
        if (!ec) {
            stamp += std::to_string(static_cast<long long>(t.time_since_epoch().count()));
        }
    }
    return stamp;
}

std::shared_ptr<const IrSet> GetRawIrs(const std::filesystem::path& aDir, bool aLowLatency)
{
    const std::string key = IrStamp(aDir) + (aLowLatency ? "|tv" : "|music");
    std::lock_guard<std::mutex> lock(gIrCacheMutex);
    auto it = gIrCache.find(key);
    if (it != gIrCache.end()) {
        return it->second;
    }
    auto set = std::make_shared<IrSet>();
    bool hasTvLeft = false;
    bool hasTvRight = false;
    if (aLowLatency) {
        hasTvLeft = LoadMonoWavIr(aDir / "ir_tv_left.wav", set->left);
        hasTvRight = LoadMonoWavIr(aDir / "ir_tv_right.wav", set->right);
    }
    if (hasTvLeft || hasTvRight) {
        set->hasLeft = true;
        if (!hasTvLeft) {
            set->left = set->right;
        }
        if (!hasTvRight) {
            set->right = set->left;
        }
    }
    else {
        set->hasLeft = LoadMonoWavIr(aDir / "left_ir.wav", set->left) ||
                       LoadMonoWavIr(aDir / "ir_left.wav", set->left);
        const bool hasRight = LoadMonoWavIr(aDir / "right_ir.wav", set->right) ||
                              LoadMonoWavIr(aDir / "ir_right.wav", set->right);
        if (set->hasLeft && !hasRight) {
            set->right = set->left;
        }
    }
    gIrCache.emplace(key, set);
    return set;
}

std::shared_ptr<const IrSet> GetIrsAtRate(const std::filesystem::path& aDir,
                                          const std::shared_ptr<const IrSet>& aRaw,
                                          uint32_t aRate,
                                          bool aLowLatency)
{
    if (aRaw->left.sampleRate == aRate && aRaw->right.sampleRate == aRate) {
        return aRaw;
    }
    const std::string key = IrStamp(aDir) + (aLowLatency ? "|tv" : "|music") +
                            "|rate=" + std::to_string(aRate);
    std::lock_guard<std::mutex> lock(gIrCacheMutex);
    auto it = gIrCache.find(key);
    if (it != gIrCache.end()) {
        return it->second;
    }
    auto set = std::make_shared<IrSet>(*aRaw);
    ResampleIrIfNeeded(set->left, aRate);
    ResampleIrIfNeeded(set->right, aRate);
    gIrCache.emplace(key, set);
    return set;
}

// Decode OpenHome Big-Endian PCM directly into normalized [-1.0, 1.0) planar doubles
template <unsigned Bytes>
inline double DecodeBe(const TByte* p)
{
    if constexpr (Bytes == 4) {
        uint32_t value;
        std::memcpy(&value, p, sizeof(value));
        if constexpr (std::endian::native == std::endian::little) {
            value = __builtin_bswap32(value);
        }
        return static_cast<double>(std::bit_cast<int32_t>(value)) *
               (1.0 / 2147483648.0);
    }
    else if constexpr (Bytes == 3) {
        const uint32_t v = (static_cast<uint32_t>(p[0]) << 16) |
                           (static_cast<uint32_t>(p[1]) << 8) |
                           static_cast<uint32_t>(p[2]);
        return static_cast<double>(SignExtend(v, 24)) * (1.0 / 8388608.0);
    }
    else if constexpr (Bytes == 2) {
        uint16_t value;
        std::memcpy(&value, p, sizeof(value));
        if constexpr (std::endian::native == std::endian::little) {
            value = __builtin_bswap16(value);
        }
        return static_cast<double>(std::bit_cast<int16_t>(value)) *
               (1.0 / 32768.0);
    }
    else {
        return static_cast<double>(SignExtend(p[0], 8)) * (1.0 / 128.0);
    }
}

template <unsigned Bytes>
void DeinterleaveImpl(const TByte* aSrc,
                      TUint aFrames,
                      TUint aNumChannels,
                      std::vector<kfr::univector<double>>& aPlanarOut)
{
    const size_t frameBytes = static_cast<size_t>(Bytes) * aNumChannels;
    for (TUint ch = 0; ch < aNumChannels; ++ch) {
        double* dst = aPlanarOut[ch].data();
        const TByte* p = aSrc + static_cast<size_t>(ch) * Bytes;
        for (TUint f = 0; f < aFrames; ++f, p += frameBytes) {
            dst[f] = DecodeBe<Bytes>(p);
        }
    }
}

void DeinterleaveBeToPlanarF64(const TByte* aSrc,
                               TUint aFrames,
                               TUint aNumChannels,
                               TUint aSubsampleBytes,
                               std::vector<kfr::univector<double>>& aPlanarOut)
{
    for (TUint ch = 0; ch < aNumChannels; ++ch) {
        aPlanarOut[ch].resize(aFrames);
    }
    switch (aSubsampleBytes) {
    case 1: DeinterleaveImpl<1>(aSrc, aFrames, aNumChannels, aPlanarOut); break;
    case 2: DeinterleaveImpl<2>(aSrc, aFrames, aNumChannels, aPlanarOut); break;
    case 3: DeinterleaveImpl<3>(aSrc, aFrames, aNumChannels, aPlanarOut); break;
    case 4: DeinterleaveImpl<4>(aSrc, aFrames, aNumChannels, aPlanarOut); break;
    default:
        for (TUint ch = 0; ch < aNumChannels; ++ch) {
            std::fill(aPlanarOut[ch].begin(), aPlanarOut[ch].end(), 0.0);
        }
        break;
    }
}

inline int32_t DoubleToS32(double aVal)
{
    if (!std::isfinite(aVal)) {
        return 0;
    }
    const double clamped = std::clamp(aVal, -1.0, 1.0);
    const double scale = clamped < 0.0 ? 2147483648.0 : 2147483647.0;
    return static_cast<int32_t>(std::lrint(clamped * scale));
}

} // namespace

class KfrPcmProcessor::KfrPcmProcPimpl
{
public:
    KfrPcmProcPimpl(IDataSink& aDataSink,
                    const AudioSpec& aSpec,
                    bool aDuplicateChannel,
                    const std::string& aIrConfigDir);

    void ProcessFragment(const Brx& aData, TUint aNumChannels, TUint aSubsampleBytes);
    void Flush();
    void Reset();
    size_t ProcessThroughSrc(TUint aChannel,
                             const kfr::univector<double>& aInput,
                             kfr::univector<double>& aOutput);

    const AudioSpec& GetSpec() const { return iSpec; }
    bool IsFlushed() const { return iFlushed; }
    TUint ComputeDelayJiffies() const;

private:
    void InterleaveAndWrite(const std::vector<kfr::univector<double>>& aChannels, size_t aFrames);

    IDataSink&  iSink;
    AudioSpec   iSpec;
    TBool       iDuplicateMono;
    bool        iFlushed{false};
    bool        iBypassSrc{false};
    bool        iFirBeforeSrc{false};
    bool        iHasFir{false};
    TUint       iProcChannels{0};   // channels after optional mono->stereo duplication

    uint32_t    iInputRate{44100};
    uint32_t    iOutputRate{44100};
    size_t      iFirDelaySamples{0};
    size_t      iConvolverDelaySamples{0};
    std::vector<size_t> iFirTailSamples;

    std::vector<kfr::samplerate_converter<double>> iConverters;
    std::vector<std::unique_ptr<kfr::convolve_filter<double>>> iFilters;

    std::vector<kfr::univector<double>> iInPlanar;
    std::vector<kfr::univector<double>> iMidPlanar;
    std::vector<kfr::univector<double>> iOutPlanar;
    std::vector<kfr::univector<double>> iSrcPending;
    std::vector<TInt32>                 iOutputSamples;
};

KfrPcmProcessor::KfrPcmProcPimpl::KfrPcmProcPimpl(
    IDataSink& aDataSink,
    const AudioSpec& aSpec,
    bool aDuplicateChannel,
    const std::string& aIrConfigDir)
: iSink(aDataSink)
, iSpec(aSpec)
, iDuplicateMono(aSpec.iNumChannels == 1 && aDuplicateChannel)
{
    if (iSpec.iInputRate <= 0.0)  iSpec.iInputRate = 44100.0;
    if (iSpec.iOutputRate <= 0.0) iSpec.iOutputRate = iSpec.iInputRate;
    if (iSpec.iNumChannels == 0)  iSpec.iNumChannels = 2;

    iInputRate  = static_cast<uint32_t>(std::llround(iSpec.iInputRate));
    iOutputRate = static_cast<uint32_t>(std::llround(iSpec.iOutputRate));
    iBypassSrc  = (iInputRate == iOutputRate);

    // Mono -> stereo happens BEFORE SRC/FIR so that left and right get their own IR
    iProcChannels = iDuplicateMono ? 2 : iSpec.iNumChannels;
    const TUint chCount = iProcChannels;
    iInPlanar.resize(chCount);
    iMidPlanar.resize(chCount);
    iOutPlanar.resize(chCount);
    iSrcPending.resize(chCount);
    // 1. Setup rational SRC per channel when inputRate != outputRate
    if (!iBypassSrc) {
        const uint32_t g = std::gcd(iOutputRate, iInputRate);
        const size_t interp = iOutputRate / g;
        const size_t decim  = iInputRate / g;
        iConverters.reserve(chCount);
        for (TUint ch = 0; ch < chCount; ++ch) {
            iConverters.emplace_back(
            kfr::sample_rate_converter<double>(
            iSpec.iLowLatency ? kfr::resample_quality::normal
                              : kfr::resample_quality::high,
            interp, decim));
        }
    }
    // 2. Load Left/Right IRs from config directory if available
    if (!aIrConfigDir.empty()) {
        const std::filesystem::path dir(aIrConfigDir);
        const auto rawIrs = GetRawIrs(dir, iSpec.iLowLatency);
        if (rawIrs->hasLeft) {
            const uint32_t irRate = rawIrs->left.sampleRate;
            if (iSpec.iLowLatency || irRate == iOutputRate) {
                iFirBeforeSrc = false;
            }
            else if (irRate == iInputRate) {
                iFirBeforeSrc = true;
            }
            else {
                iFirBeforeSrc = iInputRate < iOutputRate;
            }
            const uint32_t targetFirRate = iFirBeforeSrc ? iInputRate : iOutputRate;
            const auto irs = GetIrsAtRate(dir, rawIrs, targetFirRate, iSpec.iLowLatency);
            const LoadedIr& irLeft = irs->left;
            const LoadedIr& irRight = irs->right;
            iFirDelaySamples = std::max(irLeft.peakDelaySamples, irRight.peakDelaySamples);
            const size_t convolveBlockSize = iSpec.iLowLatency
                ? kLowLatencyConvolveBlockSize
                : kDefaultConvolveBlockSize;
            iConvolverDelaySamples = convolveBlockSize;
            iFilters.reserve(chCount);
            iFirTailSamples.resize(chCount);
            for (TUint ch = 0; ch < chCount; ++ch) {
                // Mono path (ch == 0) uses Left IR
                const LoadedIr& selectedIr = (ch == 0) ? irLeft : irRight;
                iFilters.push_back(
                std::make_unique<kfr::convolve_filter<double>>(
                selectedIr.taps, convolveBlockSize));
                iFirTailSamples[ch] = selectedIr.taps.size() - 1;
            }
            iHasFir = true;
        }
        else {
            Log::Print("KfrPcmProcessor: No readable left IR found in %s; FIR bypassed\n",
                       aIrConfigDir.c_str());
        }
    }
}

void KfrPcmProcessor::KfrPcmProcPimpl::Reset()
{
    for (auto& conv : iConverters) {
        conv.reset();
    }
    for (auto& filter : iFilters) {
        filter->reset();
    }
    for (auto& pending : iSrcPending) {
        pending.resize(0);
    }
    iFlushed = false;
}

size_t KfrPcmProcessor::KfrPcmProcPimpl::ProcessThroughSrc(
    TUint aChannel,
    const kfr::univector<double>& aInput,
    kfr::univector<double>& aOutput)
{
    auto& pending = iSrcPending[aChannel];
    auto& conv = iConverters[aChannel];

    // Common case: nothing left over from the last call, so convert straight from the input
    const kfr::univector<double>* src = &aInput;
    if (!pending.empty()) {
        const size_t oldSize = pending.size();
        pending.resize(oldSize + aInput.size());
        std::copy(aInput.begin(), aInput.end(), pending.begin() + oldSize);
        src = &pending;
    }

    const size_t outputFrames = static_cast<size_t>(conv.output_size_for_input(src->size()));
    aOutput.resize(outputFrames);
    // process() returns the number of INPUT samples consumed
    const size_t consumed = outputFrames > 0 ? conv.process(aOutput, *src) : 0;
    const size_t remaining = src->size() - consumed;

    if (src == &pending) {
        if (remaining > 0 && consumed > 0) {
            std::move(pending.begin() + consumed, pending.end(), pending.begin());
        }
        pending.resize(remaining);
    }
    else if (remaining > 0) {
        pending.resize(remaining);
        std::copy(aInput.begin() + consumed, aInput.end(), pending.begin());
    }
    return outputFrames;
}

void KfrPcmProcessor::KfrPcmProcPimpl::ProcessFragment(
    const Brx& aData, TUint aNumChannels, TUint aSubsampleBytes) {
    if (iFlushed || aData.Bytes() == 0 || aData.Ptr() == nullptr) {
        return;
    }
    const TUint bytesPerFrame = aNumChannels * aSubsampleBytes;
    if (bytesPerFrame == 0) {
        return;
    }
    const TUint inputFrames = aData.Bytes() / bytesPerFrame;
    if (inputFrames == 0) {
        return;
    }
    DeinterleaveBeToPlanarF64(aData.Ptr(), inputFrames, aNumChannels, aSubsampleBytes, iInPlanar);
    if (iDuplicateMono && aNumChannels == 1) {
        iInPlanar[1] = iInPlanar[0];
    }
    size_t finalFrames = inputFrames;
    if (iBypassSrc) {
        if (iHasFir) {
            for (TUint ch = 0; ch < iProcChannels; ++ch) {
                iOutPlanar[ch].resize(inputFrames);
                iFilters[ch]->apply(iOutPlanar[ch], iInPlanar[ch]);
            }
            InterleaveAndWrite(iOutPlanar, finalFrames);
        }
        else {
            InterleaveAndWrite(iInPlanar, finalFrames);
        }
        return;
    }
    if (iHasFir && iFirBeforeSrc) {
        // Stage 1: FIR at inputRate -> Stage 2: SRC to outputRate
        for (TUint ch = 0; ch < iProcChannels; ++ch) {
            iMidPlanar[ch].resize(inputFrames);
            iFilters[ch]->apply(iMidPlanar[ch], iInPlanar[ch]);
            const size_t outFrames = ProcessThroughSrc(ch, iMidPlanar[ch], iOutPlanar[ch]);
            finalFrames = outFrames;
        }
    }
    else if (iHasFir && !iFirBeforeSrc) {
        // Stage 1: SRC to outputRate -> Stage 2: FIR at outputRate
        for (TUint ch = 0; ch < iProcChannels; ++ch) {
            const size_t outFrames = ProcessThroughSrc(ch, iInPlanar[ch], iMidPlanar[ch]);
            if (outFrames > 0) {
                iOutPlanar[ch].resize(outFrames);
                iFilters[ch]->apply(iOutPlanar[ch], iMidPlanar[ch]);
            }
            finalFrames = outFrames;
        }
    }
    else {
    // SRC only
        for (TUint ch = 0; ch < iProcChannels; ++ch) {
            const size_t outFrames = ProcessThroughSrc(ch, iInPlanar[ch], iOutPlanar[ch]);
            finalFrames = outFrames;
        }
    }
    if (finalFrames > 0) {
        InterleaveAndWrite(iOutPlanar, finalFrames);
    }
}
void KfrPcmProcessor::KfrPcmProcPimpl::Flush()
{
    if (iFlushed) {
        return;
    }
    iFlushed = true;
    const TUint chCount = iProcChannels;
    const size_t firTailFrames = iFirTailSamples.empty()
        ? 0
        : *std::max_element(iFirTailSamples.begin(), iFirTailSamples.end());

    if (iBypassSrc) {
        if (iHasFir && firTailFrames > 0) {
            for (TUint ch = 0; ch < chCount; ++ch) {
                iOutPlanar[ch].resize(firTailFrames);
                iFilters[ch]->apply_zeros(iOutPlanar[ch]);
            }
            InterleaveAndWrite(iOutPlanar, firTailFrames);
        }
        return;
    }
    if (iConverters.empty()) {
        return;
    }

    if (iHasFir && iFirBeforeSrc && firTailFrames > 0) {
        size_t outFrames = 0;
        for (TUint ch = 0; ch < chCount; ++ch) {
            iMidPlanar[ch].resize(firTailFrames);
            iFilters[ch]->apply_zeros(iMidPlanar[ch]);
            outFrames = ProcessThroughSrc(ch, iMidPlanar[ch], iOutPlanar[ch]);
        }
        if (outFrames > 0) {
            InterleaveAndWrite(iOutPlanar, outFrames);
        }
    }

    const size_t srcDelayOut = static_cast<size_t>(std::ceil(iConverters[0].get_delay()));
    const size_t srcTailInput = std::max<size_t>(
        1, static_cast<size_t>(iConverters[0].input_size_for_output(srcDelayOut + 1)));
    size_t outFrames = 0;
    for (TUint ch = 0; ch < chCount; ++ch) {
        iInPlanar[ch].assign(srcTailInput, 0.0);
        if (iHasFir && iFirBeforeSrc) {
            iMidPlanar[ch].resize(srcTailInput);
            iFilters[ch]->apply(iMidPlanar[ch], iInPlanar[ch]);
            outFrames = ProcessThroughSrc(ch, iMidPlanar[ch], iOutPlanar[ch]);
        }
        else {
            outFrames = ProcessThroughSrc(ch, iInPlanar[ch], iMidPlanar[ch]);
            if (iHasFir && outFrames > 0) {
                iOutPlanar[ch].resize(outFrames);
                iFilters[ch]->apply(iOutPlanar[ch], iMidPlanar[ch]);
            }
            else if (!iHasFir) {
                iOutPlanar[ch] = std::move(iMidPlanar[ch]);
            }
        }
    }
    if (outFrames > 0) {
        InterleaveAndWrite(iOutPlanar, outFrames);
    }

    if (iHasFir && !iFirBeforeSrc && firTailFrames > 0) {
        for (TUint ch = 0; ch < chCount; ++ch) {
            iOutPlanar[ch].resize(firTailFrames);
            iFilters[ch]->apply_zeros(iOutPlanar[ch]);
        }
        InterleaveAndWrite(iOutPlanar, firTailFrames);
    }
}

void KfrPcmProcessor::KfrPcmProcPimpl::InterleaveAndWrite(
    const std::vector<kfr::univector<double>>& aChannels, size_t aFrames) {
    if (aFrames == 0) {
        return;
    }
    const TUint inChannels = static_cast<TUint>(aChannels.size());
    const TUint outChannels = (iDuplicateMono && inChannels == 1) ? 2 : inChannels;
    const size_t totalSamples = aFrames * outChannels;
    iOutputSamples.resize(totalSamples);
    auto* dst = iOutputSamples.data();
    if (iDuplicateMono && inChannels == 1) {
        for (size_t f = 0; f < aFrames; ++f) {
            const int32_t s = DoubleToS32(aChannels[0][f]);
            *dst++ = s;
            *dst++ = s;
        }
    }
    else {
        for (size_t f = 0; f < aFrames; ++f) {
            for (TUint ch = 0; ch < inChannels; ++ch) {
                *dst++ = DoubleToS32(aChannels[ch][f]);
            }
        }
    }
    const size_t totalBytes = totalSamples * kOutputSampleBytes;
    Brn output(reinterpret_cast<const TByte*>(iOutputSamples.data()),
               static_cast<TUint>(totalBytes));
    iSink.Write(output);
}

TUint KfrPcmProcessor::KfrPcmProcPimpl::ComputeDelayJiffies() const {
    uint64_t totalJiffies = 0;
    // SRC get_delay() is reported in output sample-rate domain
    if (!iBypassSrc && !iConverters.empty() && iOutputRate > 0) {
        const double srcDelaySamples = iConverters[0].get_delay();
        if (srcDelaySamples > 0.0) {
            totalJiffies += static_cast<uint64_t>(
            std::llround(srcDelaySamples * static_cast<double>(Jiffies::PerSample(iOutputRate))));
        }
    }
    // FIR delay is in the FIR stage's own sample-rate domain
    if (iHasFir && iFirDelaySamples > 0) {
        const uint32_t firRate = iFirBeforeSrc ? iInputRate : iOutputRate;
        if (firRate > 0) {
            totalJiffies += static_cast<uint64_t>(iFirDelaySamples) *
            static_cast<uint64_t>(Jiffies::PerSample(firRate));
        }
    }
    if (iHasFir && iConvolverDelaySamples > 0) {
        const uint32_t firRate = iFirBeforeSrc ? iInputRate : iOutputRate;
        if (firRate > 0) {
            totalJiffies += static_cast<uint64_t>(iConvolverDelaySamples) *
                            static_cast<uint64_t>(Jiffies::PerSample(firRate));
        }
    }
    return static_cast<TUint>(totalJiffies);
}

// --- Public KfrPcmProcessor ---
KfrPcmProcessor::KfrPcmProcessor(
    IDataSink& aDataSink,
    Bwx& aBuffer,
    const AudioSpec& aInitialSpec,
    const std::string& aIrConfigDir)
: iImpl(nullptr)
, iDuplicateChannel(false)
, iSink(&aDataSink)
, iBuffer(&aBuffer)
, iCurrentSpec(aInitialSpec)
, iIrConfigDir(aIrConfigDir)
, iPendingChannels(0)
, iPendingSampleBytes(0)
{}

KfrPcmProcessor::~KfrPcmProcessor() = default;

void KfrPcmProcessor::UpdateFormatSpec(const AudioSpec& aSpec) {
    std::lock_guard<std::recursive_mutex> lock(iMutex);
    if (iCurrentSpec != aSpec) {
        if (iImpl) {
            if (!iImpl->IsFlushed()) {
                iImpl->Flush();
            }
            iImpl.reset();
        }
        Clean();
        iCurrentSpec = aSpec;
        iDelayJiffies.store(0, std::memory_order_release);
    }
}
void KfrPcmProcessor::SetDuplicateChannel(TBool aDuplicateChannel) {
    std::lock_guard<std::recursive_mutex> lock(iMutex);
    if (iDuplicateChannel != aDuplicateChannel) {
        if (iImpl) {
            if (!iImpl->IsFlushed()) {
                iImpl->Flush();
            }
            iImpl.reset();
        }
        Clean();
        iDuplicateChannel = aDuplicateChannel;
        iDelayJiffies.store(0, std::memory_order_release);
    }
}

void KfrPcmProcessor::BeginBlock() {}

void KfrPcmProcessor::ProcessFragment(
    const Brx& aData, TUint aNumChannels, TUint aSubsampleBytes) {
    std::lock_guard<std::recursive_mutex> lock(iMutex);
    const TUint bytesPerFrame = aNumChannels * aSubsampleBytes;
    if (bytesPerFrame == 0 || aData.Bytes() == 0) {
        return;
    }
    if (iBuffer->Bytes() != 0 && 
       (iPendingChannels != aNumChannels || iPendingSampleBytes != aSubsampleBytes)) {
        Clean();
    }
    iPendingChannels = aNumChannels;
    iPendingSampleBytes = aSubsampleBytes;
    const TByte* dataPtr = aData.Ptr();
    TUint bytesRemaining = aData.Bytes();
    if (iBuffer->Bytes() > 0) {
        const TUint needed = bytesPerFrame - iBuffer->Bytes();
        if (bytesRemaining < needed) {
            iBuffer->Append(aData);
            return;
        }
        Brn chunk(dataPtr, needed);
        iBuffer->Append(chunk);
        dataPtr += needed;
        bytesRemaining -= needed;
        Brn completeFrame(iBuffer->Ptr(), bytesPerFrame);
        ProcessCompleteFragment(completeFrame, aNumChannels, aSubsampleBytes);
        Clean();
    }
    const TUint completeBytes = (bytesRemaining / bytesPerFrame) * bytesPerFrame;
    if (completeBytes > 0) {
        Brn completeData(dataPtr, completeBytes);
        ProcessCompleteFragment(completeData, aNumChannels, aSubsampleBytes);
        dataPtr += completeBytes;
        bytesRemaining -= completeBytes;
    }
    if (bytesRemaining > 0) {
        Brn trailingData(dataPtr, bytesRemaining);
        iBuffer->Append(trailingData);
        iPendingChannels = aNumChannels;
        iPendingSampleBytes = aSubsampleBytes;
    }
}

void KfrPcmProcessor::ProcessCompleteFragment(
const Brx& aData, TUint aNumChannels, TUint aSubsampleBytes) {
    if (aSubsampleBytes < 1 || aSubsampleBytes > 4) {
        return;
    }
    const bool sameLayout = iImpl && iImpl->GetSpec().iNumChannels == aNumChannels;
    if (sameLayout && !iImpl->IsFlushed()) {
        // stream continues
    }
    else if (sameLayout) {
        iImpl->Reset();
        iDelayJiffies.store(iImpl->ComputeDelayJiffies(), std::memory_order_release);
    }
    else {
        if (iImpl) {
            if (!iImpl->IsFlushed()) {
                iImpl->Flush();
            }
            iImpl.reset();
        }
        AudioSpec spec = iCurrentSpec;
        spec.iNumChannels = aNumChannels;
        iImpl = std::make_unique<KfrPcmProcPimpl>(
        *iSink, spec, iDuplicateChannel, iIrConfigDir);
        iDelayJiffies.store(iImpl->ComputeDelayJiffies(), std::memory_order_release);
    }
    iImpl->ProcessFragment(aData, aNumChannels, aSubsampleBytes);
}
void KfrPcmProcessor::EndBlock() {}

void KfrPcmProcessor::Flush() {
    std::lock_guard<std::recursive_mutex> lock(iMutex);
    // Keep the pimpl (converters, FFT plans, IR spectra); it is reset on the next fragment
    if (iImpl && !iImpl->IsFlushed()) {
        iImpl->Flush();
    }
    Clean();
    iDelayJiffies.store(0, std::memory_order_release);
}

double KfrPcmProcessor::GetOutputRate() const {
    std::lock_guard<std::recursive_mutex> lock(iMutex);
    return iCurrentSpec.iOutputRate;
}

TUint KfrPcmProcessor::GetDelayJiffies() const {
    return iDelayJiffies.load(std::memory_order_acquire);
}


void KfrPcmProcessor::ProcessSilence(
    const Brx& aData, TUint aNumChannels, TUint aSubsampleBytes)
{
    if (aData.Bytes() > 0) {
        ProcessFragment(aData, aNumChannels, aSubsampleBytes);
        return;
    }

    const TUint bytesPerFrame = aNumChannels * aSubsampleBytes;
    if (bytesPerFrame == 0) {
        return;
    }

    const TUint alignedBytes = 128 * bytesPerFrame;
    iSilenceBuffer.resize(alignedBytes);
    std::fill(iSilenceBuffer.begin(), iSilenceBuffer.end(), 0);
    Brn silenceData(iSilenceBuffer.data(), alignedBytes);

    ProcessFragment(silenceData, aNumChannels, aSubsampleBytes);
}
