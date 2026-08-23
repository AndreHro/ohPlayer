#pragma once

#include <OpenHome/OhNetTypes.h>
#include <OpenHome/Media/Pipeline/Msg.h>

namespace OpenHome {
namespace Media {

class IDataSink;

struct AudioSpec
{
    TUint8 iSampleBytes;      // Number of bytes per single channel sample (e.g. 2 or 4)
    TUint  iNumChannels;      // Total audio channels (1 for Mono, 2 for Stereo)
    TUint8 iBitDepth;         // The formal bit depth description metric (e.g. 16 or 24 or 32)
    double iInputRate;        // The incoming source song sample rate (e.g. 44100.0)
    double iOutputRate;       // The target upsampling rate for CamillaDSP (e.g. 192000.0)

    // A handy C++ operator override to let us compare formats in one clean step!
    bool operator==(const AudioSpec& aOther) const
    {
        return (iSampleBytes   == aOther.iSampleBytes &&
                iNumChannels   == aOther.iNumChannels &&
                iBitDepth      == aOther.iBitDepth    &&
                iInputRate     == aOther.iInputRate   &&
                iOutputRate    == aOther.iOutputRate);
    }

    bool operator!=(const AudioSpec& aOther) const
    {
        return !(*this == aOther);
    }
};

class SoxrPcmProcessor : public IPcmProcessor 
{
public:
    SoxrPcmProcessor(IDataSink& aDataSink, Bwx& aBuffer, const AudioSpec& aInitialSpec);
    ~SoxrPcmProcessor() override;

    // IPcmProcessor Interface Methods
    void BeginBlock() override;
    void ProcessFragment(const Brx& aData, TUint aNumChannels, TUint aSubsampleBytes) override;
    void ProcessSilence(const Brx& aData, TUint aNumChannels, TUint aSubsampleBytes) override;
    void EndBlock() override;
    void Flush() override;

    // Driver Configuration Hooks
    
    void SetDuplicateChannel(TBool aDuplicateChannel);
    void UpdateFormatSpec(const AudioSpec& aNewSpec);
    double GetOutputRate() const;

private:
    class SoxrPcmProcPimpl;

    void ProcessCompleteFragment(
        const Brx& aData,
        TUint aNumChannels,
        TUint aSubsampleBytes);

    std::unique_ptr<SoxrPcmProcPimpl> iImpl;

    void Clean()
    {
        iBuffer->SetBytes(0);
        iPendingChannels=0;
        iPendingSampleBytes=0;
    }
    
    bool         iDuplicateChannel;
    IDataSink*   iSink;
    Bwx*         iBuffer;
    AudioSpec    iCurrentSpec; // Added to cache parameters during early constructor execution
    TUint iPendingChannels;
    TUint iPendingSampleBytes;
};

} // namespace Media
} // namespace OpenHome
