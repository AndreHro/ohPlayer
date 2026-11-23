#pragma once

#include <OpenHome/Media/Protocol/Protocol.h>
#include <memory>

namespace OpenHome {
namespace Media {

class MsgFactory;
class IPipelineElementDownstream;

class ProtocolAlsaCapture : public Protocol
{
    static constexpr TUint     kDefaultSampleRate  = 44100;
    static constexpr TUint     kDefaultChannels    = 2;
public:
    struct DevParam
    {
        Bws<64> iDevName{"hw:2,0"};
        TUint iSampleRate{kDefaultSampleRate};
        TUint iChannels{kDefaultChannels};
    };

    ProtocolAlsaCapture(const DevParam& aDevData, Environment& aEnv);
    ~ProtocolAlsaCapture() override;

private: // from Protocol
    ProtocolStreamResult Stream(const Brx& aUri) override;
    void Interrupt(TBool aInterrupt) override; 
    void Initialise(MsgFactory& aMsgFactory, IPipelineElementDownstream& aDownstream) override;
    ProtocolGetResult Get(IWriter& aWriter, const Brx& aUri, TUint64 aOffset, TUint aBytes) override;
    TUint TryStop(TUint aStreamId) override;

private:
    class Pimpl;
    std::unique_ptr<Pimpl> iImpl;
};

} // namespace Media
} // namespace OpenHome
