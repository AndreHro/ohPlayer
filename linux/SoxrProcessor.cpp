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
    IDataSink&      iSink;
    soxr_t          iInstance;
    soxr_error_t    iError;
    AudioSpec       iSpec;
    soxr_datatype_t iInputType;
    TUint           iTargetChannels;
};

SoxrPcmProcessor::SoxrPcmProcPimpl::SoxrPcmProcPimpl(IDataSink& aDataSink, const AudioSpec& aSpec, soxr_datatype_t aInputType, bool aDuplicateChannel)
: iSink(aDataSink)
, iInstance(nullptr)
, iError(nullptr)
, iSpec(aSpec)
, iInputType(aInputType)
, iTargetChannels(0)
{
    // Guard against zero division errors if an invalid spec leaks through
    if (iSpec.iInputRate <= 0.0)  iSpec.iInputRate = 44100.0;
    if (iSpec.iOutputRate <= 0.0) iSpec.iOutputRate = iSpec.iInputRate;
    if (iSpec.iNumChannels == 0)  iSpec.iNumChannels = 2;

    soxr_io_spec_t ioSpec = soxr_io_spec(iInputType, SOXR_INT32_I);
    soxr_quality_spec_t qSpec = soxr_quality_spec(SOXR_HQ, 0);

    if (iSpec.iNumChannels == 1 && aDuplicateChannel) {
        iTargetChannels = 2;
    }
    else {
        iTargetChannels = iSpec.iNumChannels;
    }

    iInstance = soxr_create(
        iSpec.iInputRate,
        iSpec.iOutputRate,
        iTargetChannels,
        &iError,
        &ioSpec,
        &qSpec,
        nullptr);;

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
    const TBool duplicateMono = (aNumChannels == 1 && iTargetChannels == 2);
    const Brx* inputData = &aData;
    std::vector<TByte> expandedInput;
    Brn expandedWrapper;

    if (duplicateMono) {
        expandedInput.resize(inputFrames * 2 * aSubsampleBytes);

        const TByte* source = aData.Ptr();
        TByte* destination = expandedInput.data();

        for (size_t frame = 0; frame < inputFrames; ++frame) {
            std::memcpy(destination, source, aSubsampleBytes);
            destination += aSubsampleBytes;

            std::memcpy(destination, source, aSubsampleBytes);
            destination += aSubsampleBytes;

            source += aSubsampleBytes;
        }

        expandedWrapper.Set(expandedInput.data(), expandedInput.size());
        inputData = &expandedWrapper;
    }

    const double ratio = iSpec.iOutputRate / iSpec.iInputRate;
    const size_t maxOutputFrames =
        std::max<size_t>(
            1,
            static_cast<size_t>(std::ceil(inputFrames * ratio)) + 32);
    
            const TUint bytesPerOutputFrame = iTargetChannels * 4;
    std::vector<TByte> outputBuffer(maxOutputFrames * bytesPerOutputFrame);
    size_t outputFramesProcessed = 0;

    iError = soxr_process(iInstance,
                          inputData->Ptr(),
                          inputFrames,
                          nullptr,
                          outputBuffer.data(),
                          maxOutputFrames,
                          &outputFramesProcessed);
    if (iError) {
        Log::Print("SoxrPcmProcessor: Processing Error: %s\n",
                   soxr_strerror(iError));
        return;
    }

    if (outputFramesProcessed != 0) {
        Brn output(outputBuffer.data(),
                   outputFramesProcessed * bytesPerOutputFrame);
        iSink.Write(output);
    }
}

void SoxrPcmProcessor::SoxrPcmProcPimpl::FlushResamplerCache()
{
    if (!iInstance) {
        return;
    }

    constexpr size_t flushFrames = 2048;
    const TUint bytesPerOutputFrame = iTargetChannels * 4;
    std::vector<TByte> buffer(flushFrames * bytesPerOutputFrame);

    for (;;) {
        size_t outputFrames = 0;

        iError = soxr_process(iInstance,
                              nullptr,
                              0,
                              nullptr,
                              buffer.data(),
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

        Brn output(buffer.data(), outputFrames * bytesPerOutputFrame);
        iSink.Write(output);
    }
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
    std::vector<TByte> remainder(remainingBytes);

    std::memcpy(
        remainder.data(),
        iBuffer->Ptr() + completeBytes,
        remainingBytes);

    // Rebuild the buffer using the supported Bwx API.
    Clean();

    Brn remainderData(remainder.data(), remainingBytes);
    iBuffer->Append(remainderData);

    iPendingChannels = aNumChannels;
    iPendingSampleBytes = aSubsampleBytes;
}

void SoxrPcmProcessor::ProcessCompleteFragment(const Brx& aData, TUint aNumChannels, TUint aSubsampleBytes)
{
    soxr_datatype_t inputType;
    std::vector<TByte> conversionBuffer;
    const Brx* dataToProcess = &aData;
    Brn localThreadWrapper; // Fixed: Thread-safe local scope handling

    switch(aSubsampleBytes) {
        case 1: {
            // Safe 8-bit to 16-bit expansion mapping
            inputType = SOXR_INT16_I;
            TUint numSamples = aData.Bytes();
            conversionBuffer.resize(numSamples * 2);
            const TUint8* src = reinterpret_cast<const TUint8*>(aData.Ptr());
            TInt16* dest = reinterpret_cast<TInt16*>(conversionBuffer.data());
            
            for (TUint i = 0; i < numSamples; ++i) {
                // Convert unsigned 8-bit offset to standard signed 16-bit PCM
                dest[i] = static_cast<TInt16>((static_cast<TInt32>(src[i]) - 128) << 8);
            }
            localThreadWrapper.Set(conversionBuffer.data(), conversionBuffer.size());
            dataToProcess = &localThreadWrapper;
            break;
        }
        case 2: 
            inputType = SOXR_INT16_I; 
            break;
        case 3: {
            // Sign-extended bit-perfect 24-bit to 32-bit container expansion
            inputType = SOXR_INT32_I;
            TUint numSamples = aData.Bytes() / 3;
            conversionBuffer.resize(numSamples * 4);
            const TByte* src = aData.Ptr();
            TByte* dest = conversionBuffer.data();
            
            for (TUint i = 0; i < numSamples; ++i) {
                // Read 3 raw bytes explicitly
                TUint32 val = (static_cast<TUint32>(src[0])) |
                              (static_cast<TUint32>(src[1]) << 8) |
                              (static_cast<TUint32>(src[2]) << 16);

                // Sign-extend negative 24-bit samples to 32-bit values
                if (val & 0x800000) {
                    val |= 0xFF000000;
                }

                // Align bits to the MSB side for standard 32-bit PCM audio pipelines
                val <<= 8; 

                std::memcpy(dest, &val, 4);
                src += 3;
                dest += 4;
            }
            localThreadWrapper.Set(conversionBuffer.data(), conversionBuffer.size());
            dataToProcess = &localThreadWrapper;
            break;
        }
        case 4: 
            inputType = SOXR_INT32_I; 
            break;
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
        iCurrentSpec.iNumChannels = aNumChannels;
        iImpl = std::make_unique<SoxrPcmProcPimpl>(
            *iSink, iCurrentSpec, inputType, iDuplicateChannel);
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
