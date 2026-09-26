#include "SoxrProcessor.h"
#include "IDataSink.h"
#include <OpenHome/Private/Printer.h>

#include <soxr.h>
#include <cmath>
#include <cstring>
#include <memory>
#include <vector>
#include <algorithm>

using namespace OpenHome;
using namespace OpenHome::Media;

namespace {
    constexpr TUint kOutputSampleBytes = sizeof(TInt32);
}

class SoxrPcmProcessor::SoxrPcmProcPimpl
{
public:
    SoxrPcmProcPimpl(IDataSink& aDataSink, const AudioSpec& aSpec, soxr_datatype_t aInputType, bool aDuplicateChannel);
    ~SoxrPcmProcPimpl()
    {
        if (iInstance) {
            soxr_delete(iInstance);
        }
    }

    void ProcessFragmentWithSoxr(const Brx& aData, TUint aNumChannels, TUint aSubsampleBytes);
    void FlushResamplerCache();

    const AudioSpec& GetSpec() const { return iSpec; }
    soxr_datatype_t GetInputType() const { return iInputType; }

private:

    void WriteMonoAsStereo(const TByte* aSource, size_t aFrames);

    IDataSink&      iSink;
    soxr_t          iInstance;
    soxr_error_t    iError;
    AudioSpec       iSpec;
    soxr_datatype_t iInputType;
    TBool           iDuplicateMono;
    std::vector<TByte> iStereoBuffer;
    std::vector<TByte> iOutputBuffer;
};

SoxrPcmProcessor::SoxrPcmProcPimpl::SoxrPcmProcPimpl(IDataSink& aDataSink, const AudioSpec& aSpec, soxr_datatype_t aInputType, bool aDuplicateChannel)
: iSink(aDataSink)
, iInstance(nullptr)
, iError(nullptr)
, iSpec(aSpec)
, iInputType(aInputType)
, iDuplicateMono(aSpec.iNumChannels == 1 && aDuplicateChannel)
, iStereoBuffer()
, iOutputBuffer()
{
    // Guard against zero division errors if an invalid spec leaks through
    if (iSpec.iInputRate <= 0.0)  iSpec.iInputRate = 44100.0;
    if (iSpec.iOutputRate <= 0.0) iSpec.iOutputRate = iSpec.iInputRate;
    if (iSpec.iNumChannels == 0)  iSpec.iNumChannels = 2;

    soxr_io_spec_t ioSpec = soxr_io_spec(iInputType, SOXR_INT32_I);
    soxr_quality_spec_t qSpec = soxr_quality_spec(SOXR_HQ, 0);

    // Maintain symmetric channel topology natively requested by the stream
    // If input is mono, libsoxr processes it as pure 1-channel mono
    iInstance = soxr_create(
        iSpec.iInputRate,
        iSpec.iOutputRate,
        iSpec.iNumChannels,
        &iError,
        &ioSpec,
        &qSpec,
        nullptr);

    if (iError || !iInstance) {
        Log::Print("SoxrPcmProcessor: Initialization Failed! Error Token: %s\n", soxr_strerror(iError));
    }
    ASSERT(iInstance != nullptr);
}

void SoxrPcmProcessor::SoxrPcmProcPimpl::ProcessFragmentWithSoxr(
    const Brx& aData, TUint aNumChannels, TUint aSubsampleBytes)
{
    const TUint bytesPerInputFrame = aNumChannels * aSubsampleBytes;
    if (bytesPerInputFrame == 0) {
        return;
    }

    const size_t inputFrames = aData.Bytes() / bytesPerInputFrame;
    if (inputFrames == 0) {
        return;
    }

    const double ratio = iSpec.iOutputRate / iSpec.iInputRate;
    const size_t maxOutputFrames =
        std::max<size_t>(
            1,
            static_cast<size_t>(std::ceil(inputFrames * ratio)) + 32);
    
    // The resampler operates symmetrically. Output width matches internal channels (always 1 for mono)
    // The target data type is hard-locked to SOXR_INT32_I, which means exactly 4 bytes per sample.
    const TUint bytesPerResamplerFrame =
        iSpec.iNumChannels * kOutputSampleBytes;
    iOutputBuffer.resize(maxOutputFrames * bytesPerResamplerFrame);
    
    size_t outputFramesProcessed = 0;

    iError = soxr_process(iInstance,
                          aData.Ptr(),
                          inputFrames,
                          nullptr,
                          iOutputBuffer.data(),
                          maxOutputFrames,
                          &outputFramesProcessed);
    if (iError) {
        Log::Print("SoxrPcmProcessor: Processing Error: %s\n",
                    soxr_strerror(iError));
        return;
    }

    if (outputFramesProcessed != 0) {
        if (iDuplicateMono) {
            WriteMonoAsStereo(iOutputBuffer.data(), outputFramesProcessed);
        }
        else {
            const TUint outputBytes = outputFramesProcessed * bytesPerResamplerFrame;
            Brn output(iOutputBuffer.data(), outputBytes);
            iSink.Write(output);
        }
    }
}

void SoxrPcmProcessor::SoxrPcmProcPimpl::FlushResamplerCache()
{
    if (!iInstance) {
        return;
    }

    constexpr size_t flushFrames = 2048;
    const TUint resamplerChannels = iSpec.iNumChannels;
    const TUint bytesPerResamplerFrame = resamplerChannels * kOutputSampleBytes; // Always 4 bytes per sample (INT32)
    iOutputBuffer.resize(flushFrames * bytesPerResamplerFrame);

    for (;;) {
        size_t outputFrames = 0;

        iError = soxr_process(iInstance,
                              nullptr,
                              0,
                              nullptr,
                              iOutputBuffer.data(),
                              flushFrames,
                              &outputFrames);

        if (iError) {
            Log::Print("SoxrPcmProcessor: Flush Error: %s\n",
                    soxr_strerror(iError));
            return;
        }
        if (outputFrames == 0) {
            break;
        }
        if (iDuplicateMono) {
            WriteMonoAsStereo(iOutputBuffer.data(), outputFrames);
        }
        else {
            const TUint outputBytes = outputFrames * bytesPerResamplerFrame;
            Brn output(iOutputBuffer.data(), outputBytes);
            iSink.Write(output);
        }
    }
}

void SoxrPcmProcessor::SoxrPcmProcPimpl::WriteMonoAsStereo(const TByte* aSource, size_t aFrames)
{
    const TUint stereoFrameBytes = 2 * kOutputSampleBytes;
    const auto newSize = aFrames * stereoFrameBytes;
    iStereoBuffer.resize(newSize);

    TByte* destination = iStereoBuffer.data();
    for (size_t frame = 0; frame < aFrames; ++frame) {
        std::memcpy(destination, aSource, kOutputSampleBytes);
        std::memcpy(destination + kOutputSampleBytes, aSource, kOutputSampleBytes);

        aSource += kOutputSampleBytes;
        destination += stereoFrameBytes;
    }

    Brn output(iStereoBuffer.data(), newSize);
    iSink.Write(output);
}

SoxrPcmProcessor::SoxrPcmProcessor(
    IDataSink& aDataSink,
    Bwx& aBuffer,
    const AudioSpec& aInitialSpec)
: iImpl(nullptr)
, iDuplicateChannel(false)
, iSink(&aDataSink)
, iBuffer(&aBuffer)
, iCurrentSpec(aInitialSpec)
, iPendingChannels(0)
, iPendingSampleBytes(0)
{
}

SoxrPcmProcessor::~SoxrPcmProcessor() {}

void SoxrPcmProcessor::UpdateFormatSpec(const AudioSpec& aSpec)
{
    if (iCurrentSpec!=aSpec) {
        if (iImpl) {
            iImpl->FlushResamplerCache();
            iImpl.reset();
        }

        Clean();
        iCurrentSpec = aSpec;
    }
}

void SoxrPcmProcessor::SetDuplicateChannel(TBool aDuplicateChannel)
{
    if (iDuplicateChannel != aDuplicateChannel) {
        if (iImpl) {
            iImpl->FlushResamplerCache();
            iImpl.reset();
        }

        Clean();
        iDuplicateChannel = aDuplicateChannel;
    }
}

void SoxrPcmProcessor::ProcessFragment(
    const Brx& aData,
    TUint aNumChannels,
    TUint aSubsampleBytes)
{
    const TUint bytesPerFrame = aNumChannels * aSubsampleBytes;

    if (bytesPerFrame == 0) {
        return;
    }

    if (iBuffer->Bytes() != 0 &&
        (iPendingChannels != aNumChannels ||
        iPendingSampleBytes != aSubsampleBytes)) {
        Clean();
    }

    iPendingChannels = aNumChannels;
    iPendingSampleBytes = aSubsampleBytes;
    iBuffer->Append(aData);

    const TUint completeBytes = 
        (iBuffer->Bytes() / bytesPerFrame) * bytesPerFrame;

    const TUint remainingBytes = iBuffer->Bytes() - completeBytes;

    if (completeBytes != 0) {
        Brn completeData(iBuffer->Ptr(), completeBytes);
        ProcessCompleteFragment(
            completeData,
            aNumChannels,
            aSubsampleBytes);
    }

    if (remainingBytes == 0) {
        Clean();
        return;
    }

    // Copy the incomplete trailing frame before changing iBuffer.
    iRemainderBuffer.resize(remainingBytes);

    std::memcpy(iRemainderBuffer.data(),
                iBuffer->Ptr() + completeBytes,
                remainingBytes);

    Clean();

    Brn remainderData(iRemainderBuffer.data(), remainingBytes);
    iBuffer->Append(remainderData);

    iPendingChannels = aNumChannels;
    iPendingSampleBytes = aSubsampleBytes;
}

void SoxrPcmProcessor::ProcessCompleteFragment(const Brx& aData, TUint aNumChannels, TUint aSubsampleBytes)
{
    soxr_datatype_t inputType;
    const Brx* dataToProcess = &aData;
    Brn localThreadWrapper; // Fixed: Thread-safe local scope handling
    
    switch(aSubsampleBytes) {
    case 1: {
        // Safe 8-bit to 16-bit expansion mapping
        inputType = SOXR_INT16_I;

        const TUint numSamples = aData.Bytes();
        iInputBuffer.resize(numSamples * sizeof(TInt16));

        const TByte* src = aData.Ptr();
        TByte* dest = iInputBuffer.data();

        for (TUint i = 0; i < numSamples; ++i) {
            const TInt16 sample =
                static_cast<TInt16>(
                    (static_cast<TInt32>(src[i]) - 128) << 8);

            std::memcpy(dest + i * sizeof(TInt16), &sample, sizeof(sample));
        }

        localThreadWrapper.Set(iInputBuffer.data(), iInputBuffer.size());
        dataToProcess = &localThreadWrapper;
        break;
    }
    case 2: {
        inputType = SOXR_INT16_I;

        const TUint numSamples = aData.Bytes() / 2;
        iInputBuffer.resize(numSamples * sizeof(TInt16));

        const TByte* src = aData.Ptr();
        TByte* dest = iInputBuffer.data();

        for (TUint i = 0; i < numSamples; ++i) {
            // Input is packed big-endian; soxr expects native-endian PCM.
            const TInt16 sample = static_cast<TInt16>(
                (static_cast<TUint16>(src[0]) << 8) |
                static_cast<TUint16>(src[1]));

            std::memcpy(dest + i * sizeof(TInt16), &sample, sizeof(sample));
            src += 2;
        }

        localThreadWrapper.Set(iInputBuffer.data(), iInputBuffer.size());
        dataToProcess = &localThreadWrapper;
        break;
    }

    case 3: {
        inputType = SOXR_INT32_I;

        const TUint numSamples = aData.Bytes() / 3;
        iInputBuffer.resize(numSamples * sizeof(TInt32));

        const TByte* src = aData.Ptr();
        TByte* dest = iInputBuffer.data();

        for (TUint i = 0; i < numSamples; ++i) {
            TUint32 value =
                (static_cast<TUint32>(src[0]) << 16) |
                (static_cast<TUint32>(src[1]) << 8) |
                static_cast<TUint32>(src[2]);

            if ((value & 0x00800000u) != 0) {
                value |= 0xFF000000u;
            }

            // Preserve the existing 24-bit-to-32-bit left alignment.
            value <<= 8;

            std::memcpy(dest + i * sizeof(TInt32), &value, sizeof(value));
            src += 3;
        }

        localThreadWrapper.Set(iInputBuffer.data(), iInputBuffer.size());
        dataToProcess = &localThreadWrapper;
        break;
    }

    case 4: {
        inputType = SOXR_INT32_I;

        const TUint numSamples = aData.Bytes() / 4;
        iInputBuffer.resize(numSamples * sizeof(TInt32));

        const TByte* src = aData.Ptr();
        TByte* dest = iInputBuffer.data();

        for (TUint i = 0; i < numSamples; ++i) {
            const TUint32 value =
                (static_cast<TUint32>(src[0]) << 24) |
                (static_cast<TUint32>(src[1]) << 16) |
                (static_cast<TUint32>(src[2]) << 8) |
                static_cast<TUint32>(src[3]);

            std::memcpy(dest + i * sizeof(TInt32), &value, sizeof(value));
            src += 4;
        }

        localThreadWrapper.Set(iInputBuffer.data(), iInputBuffer.size());
        dataToProcess = &localThreadWrapper;
        break;
    }
    default:
        return;
    }

    const TUint effectiveSampleBytes =
        (aSubsampleBytes == 1) ? 2 :
        (aSubsampleBytes == 3) ? 4 :
        aSubsampleBytes;

    if (!iImpl || iCurrentSpec.iNumChannels != aNumChannels ||
        iImpl->GetInputType() != inputType)
    {
        if (iImpl) {
            iImpl->FlushResamplerCache();
            iImpl.reset();
        }
        iCurrentSpec.iNumChannels = aNumChannels;
        iImpl = std::make_unique<SoxrPcmProcPimpl>(
            *iSink, iCurrentSpec, inputType, iDuplicateChannel);
        if (!iImpl) {
            return;
        }
    }

    iImpl->ProcessFragmentWithSoxr(
        *dataToProcess,
        aNumChannels,
        effectiveSampleBytes);
}

void SoxrPcmProcessor::EndBlock()
{
    if (iImpl) {
        iImpl->FlushResamplerCache();
    }

    Clean();
}

void SoxrPcmProcessor::Flush()
{
    if (iImpl) {
        iImpl->FlushResamplerCache();
    }

    Clean();
}

double SoxrPcmProcessor::GetOutputRate() const
{
    return iCurrentSpec.iOutputRate;
}

void SoxrPcmProcessor::BeginBlock()
{
    Clean();
}

void SoxrPcmProcessor::ProcessSilence(
    const Brx& aData, TUint aNumChannels, TUint aSubsampleBytes)
{
    ProcessFragment(aData, aNumChannels, aSubsampleBytes);
}
