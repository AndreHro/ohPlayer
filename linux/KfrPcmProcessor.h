#pragma once

#include <OpenHome/Media/Pipeline/Msg.h>
#include <OpenHome/Private/Standard.h>
#include <OpenHome/Types.h>
#include <OpenHome/Buffer.h>

#include <atomic>
#include <memory>
#include <string>
#include <vector>
#include <mutex>

namespace OpenHome {
namespace Media {

class IDataSink;

struct AudioSpec
{
    TUint  iNumChannels{2};
    TUint  iBitDepth{32};
    double iInputRate{44100.0};
    double iOutputRate{44100.0};
    bool   iLowLatency{false};

    bool operator==(const AudioSpec& aOther) const
    {
        return iNumChannels == aOther.iNumChannels &&
               iBitDepth    == aOther.iBitDepth &&
               iInputRate   == aOther.iInputRate &&
               iOutputRate  == aOther.iOutputRate &&
               iLowLatency  == aOther.iLowLatency;
    }
    bool operator!=(const AudioSpec& aOther) const { return !(*this == aOther); }
};

class KfrPcmProcessor : public IPcmProcessor
{
public:
    KfrPcmProcessor(IDataSink& aDataSink,
                    Bwx& aBuffer,
                    const AudioSpec& aInitialSpec,
                    const std::string& aIrConfigDir);
    ~KfrPcmProcessor() override;

    void UpdateFormatSpec(const AudioSpec& aSpec);
    void SetDuplicateChannel(TBool aDuplicateChannel);
    double GetOutputRate() const;
    TUint GetDelayJiffies() const;

    // IPcmProcessor
    void BeginBlock() override;
    void ProcessFragment(const Brx& aData, TUint aNumChannels, TUint aSubsampleBytes) override;
    void ProcessSilence(const Brx& aData, TUint aNumChannels, TUint aSubsampleBytes) override;
    void EndBlock() override;
    void Flush() override;

private:
    void Clean() { iBuffer->SetBytes(0); }
    void ProcessCompleteFragment(const Brx& aData, TUint aNumChannels, TUint aSubsampleBytes);

    class KfrPcmProcPimpl;
    std::unique_ptr<KfrPcmProcPimpl> iImpl;

    TBool        iDuplicateChannel;
    IDataSink*   iSink;
    Bwx*         iBuffer;
    AudioSpec    iCurrentSpec;
    std::string  iIrConfigDir;
    TUint        iPendingChannels;
    TUint        iPendingSampleBytes;
    std::atomic<TUint> iDelayJiffies{0};
    mutable std::recursive_mutex iMutex;
    std::vector<TByte> iSilenceBuffer;
};

} // namespace Media
} // namespace OpenHome
