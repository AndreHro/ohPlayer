#include "SoxrProcessor.h"
#include "IDataSink.h"
#include "ConvertBELE.h"
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
            iInstance = nullptr;
        }
    }

    void ProcessFragmentWithSoxr(const Brx& aData, TUint aNumChannels, TUint aSubsampleBytes);
    void FlushResamplerCache();

    const AudioSpec& GetSpec() const { return iSpec; }
    soxr_datatype_t GetInputType() const { return iInputType; }
    bool IsFlushed() const { return iFlushed; }

private:

    void WriteMonoAsStereo(const TByte* aSource, TUint aFrames);

    IDataSink&      iSink;
    soxr_t          iInstance;
    soxr_error_t    iError;
    AudioSpec       iSpec;
    soxr_datatype_t iInputType;
    TBool           iDuplicateMono;
    bool            iFlushed;
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
, iFlushed(false)
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
    if (iFlushed || !iInstance) {
        return;
    }

    if (aData.Bytes() == 0 || aData.Ptr() == nullptr) {
        return; 
    }

    const TUint bytesPerInputFrame = aNumChannels * aSubsampleBytes;
    if (bytesPerInputFrame == 0) {
        return;
    }

    const TUint inputFrames = aData.Bytes() / bytesPerInputFrame;
    if (inputFrames == 0) {
        return;
    }

    const double ratio = iSpec.iOutputRate / iSpec.iInputRate;
    const TUint maxOutputFrames =
        std::max<TUint>(
            1,
            static_cast<TUint>(std::ceil(inputFrames * ratio)) + 32);
    
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
    if (!iInstance || iFlushed) {
        return;
    }

    iFlushed = true;

    constexpr TUint flushFrames = 2048;
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

void SoxrPcmProcessor::SoxrPcmProcPimpl::WriteMonoAsStereo(const TByte* aSource, TUint aFrames)
{
    const TUint stereoFrameBytes = 2 * kOutputSampleBytes;
    const auto newSize = aFrames * stereoFrameBytes;
    iStereoBuffer.resize(newSize);

    TByte* destination = iStereoBuffer.data();
    for (TUint frame = 0; frame < aFrames; ++frame) {
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
    if (iCurrentSpec != aSpec) {
        if (iImpl) {
            if (!iImpl->IsFlushed()) {
                iImpl->FlushResamplerCache();
            }
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
            if (!iImpl->IsFlushed()) {
                iImpl->FlushResamplerCache();
            }
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
    if (bytesPerFrame == 0 || aData.Bytes() == 0) {
        return;
    }

    if (iBuffer->Bytes() != 0 &&
        (iPendingChannels != aNumChannels ||
         iPendingSampleBytes != aSubsampleBytes)) {
        Clean();
    }

    iPendingChannels = aNumChannels;
    iPendingSampleBytes = aSubsampleBytes;

    const TByte* dataPtr = aData.Ptr();
    TUint bytesRemaining = aData.Bytes();

    // If there are leftover bytes from a previous fragment, complete that frame first
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

    // Process complete frames directly from input without extra buffering
    const TUint completeBytes = (bytesRemaining / bytesPerFrame) * bytesPerFrame;
    if (completeBytes > 0) {
        Brn completeData(dataPtr, completeBytes);
        ProcessCompleteFragment(completeData, aNumChannels, aSubsampleBytes);
        dataPtr += completeBytes;
        bytesRemaining -= completeBytes;
    }

    // Save any trailing incomplete frame
    if (bytesRemaining > 0) {
        Brn trailingData(dataPtr, bytesRemaining);
        iBuffer->Append(trailingData);
        iPendingChannels = aNumChannels;
        iPendingSampleBytes = aSubsampleBytes;
    }
}

void SoxrPcmProcessor::ProcessCompleteFragment(const Brx& aData, TUint aNumChannels, TUint aSubsampleBytes)
{
    if (aSubsampleBytes < 1 || aSubsampleBytes > 4) {
        return;
    }
    
    const TUint bytesPerFrame = aNumChannels * aSubsampleBytes;
    if (bytesPerFrame == 0 || aData.Bytes() < bytesPerFrame || aData.Ptr() == nullptr) {
        return;
    }

    const TUint inputFrames = aData.Bytes() / bytesPerFrame;
    if (inputFrames == 0) {
        return;
    }

    const TUint totalSamples = inputFrames * aNumChannels;
    const TUint s32Bytes = totalSamples * sizeof(TInt32);
    iInputBuffer.resize(s32Bytes);

    const size_t convertedFrames = OpenHome::Media::ConvertInterleavedToS32LE_With24Simd(
        aData.Ptr(),
        aData.Bytes(),
        reinterpret_cast<TUint8*>(iInputBuffer.data()),
        inputFrames,
        aNumChannels,
        aSubsampleBytes,
        false /* sourceLittleEndian: OpenHome pipeline is Big-Endian */,
        false /* duplicateMono: handled after resampling */,
        iTmpOut);

    if (convertedFrames == 0) {
        return;
    }

    const TUint convertedBytes = convertedFrames * aNumChannels * sizeof(TInt32);
    Brn convertedData(iInputBuffer.data(), convertedBytes);

    if (!iImpl || iImpl->IsFlushed() ||
        iCurrentSpec.iNumChannels != aNumChannels ||
        iImpl->GetInputType() != SOXR_INT32_I)
    {
        if (iImpl) {
            if (!iImpl->IsFlushed()) {
                iImpl->FlushResamplerCache();
            }
            iImpl.reset();
        }
        iCurrentSpec.iNumChannels = aNumChannels;
        iImpl = std::make_unique<SoxrPcmProcPimpl>(
            *iSink, iCurrentSpec, SOXR_INT32_I, iDuplicateChannel);
        if (!iImpl) {
            return;
        }
    }

    iImpl->ProcessFragmentWithSoxr(
        convertedData,
        aNumChannels,
        sizeof(TInt32));
}

void SoxrPcmProcessor::EndBlock()
{
    // Continuous audio pipeline: MsgPlayable invokes BeginBlock/ProcessFragment/EndBlock
    // on every audio message. Do not flush the resampler or discard remainder frames here.
}

void SoxrPcmProcessor::Flush()
{
    if (iImpl) {
        if (!iImpl->IsFlushed()) {
            iImpl->FlushResamplerCache();
        }
        iImpl.reset();
    }

    Clean();
}

double SoxrPcmProcessor::GetOutputRate() const
{
    return iCurrentSpec.iOutputRate;
}

void SoxrPcmProcessor::BeginBlock()
{
    // No-op to preserve continuous audio streaming across playable blocks.
}

void SoxrPcmProcessor::ProcessSilence(
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
