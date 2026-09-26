#ifndef HEADER_PIPELINE_DRIVER_ALSA
#define HEADER_PIPELINE_DRIVER_ALSA

#include <OpenHome/OhNetTypes.h>
#include <OpenHome/Media/Pipeline/Msg.h>
#include <OpenHome/Media/Utils/ProcessorAudioUtils.h>
#include <OpenHome/Private/Thread.h>



namespace OpenHome {
namespace Media {

class PriorityArbitratorDriver : public IPriorityArbitrator, private INonCopyable
{
public:
    PriorityArbitratorDriver(TUint aOpenHomeMax);
private: // from IPriorityArbitrator
    TUint Priority(const TChar* aId, TUint aRequested, TUint aHostMax) override;
    TUint OpenHomeMin() const override;
    TUint OpenHomeMax() const override;
    TUint HostRange() const override;
private:
    const TUint iOpenHomeMax;
};

class IDriverBackend
{
public:
    virtual ~IDriverBackend() = default;

    virtual void ProcessDecodedStream(MsgDecodedStream*) = 0;
    virtual void ProcessPlayable(MsgPlayable*) = 0;
    virtual void ProcessDrain() = 0;
    virtual void ProcessMode() = 0;
    virtual TUint DriverDelayJiffies(TUint aSampleRate) = 0;
};

class PipelineDriverBase
    : public PipelineElement
    , public IPipelineAnimator
    , private INonCopyable
{
protected:
    PipelineDriverBase(IPipeline& aPipeline,
                       std::unique_ptr<IDriverBackend> aBackend);

    ~PipelineDriverBase();

    void AudioThread();

    TUint GetSupportedElements()
    {
        return
        PipelineElement::MsgType::eMode |
        PipelineElement::MsgType::eDrain |
        PipelineElement::MsgType::eHalt |
        PipelineElement::MsgType::eDecodedStream |
        PipelineElement::MsgType::ePlayable |
        PipelineElement::MsgType::eQuit;
    }

private:
    Msg* ProcessMsg(MsgMode*) override;
    Msg* ProcessMsg(MsgDrain*) override;
    Msg* ProcessMsg(MsgHalt*) override;
    Msg* ProcessMsg(MsgDecodedStream*) override;
    Msg* ProcessMsg(MsgPlayable*) override;
    Msg* ProcessMsg(MsgQuit*) override;

    TUint PipelineAnimatorBufferJiffies() const override;
    TUint PipelineAnimatorDelayJiffies(
        AudioFormat, TUint, TUint, TUint) const override;
    void PipelineAnimatorDsdBlockConfiguration(
        TUint&, TUint&) const override;
    TUint PipelineAnimatorMaxBitDepth() const override;
    void PipelineAnimatorGetMaxSampleRates(
        TUint&, TUint&) const override;

protected:
    IPipeline& iPipeline;
    Mutex iMutex;
    TBool iQuit;
    std::unique_ptr<IDriverBackend> iBackend;
    std::unique_ptr<ThreadFunctor> iThread;
};

class DriverAlsa final : public PipelineDriverBase
{
public:
    DriverAlsa(IPipeline& aPipeline, const Brx& aAlsaDevice, TUint aBufferUs, TUint aOutputSampleRate=0);
};
class CamillaDspDriver final : public PipelineDriverBase
{
public:
    CamillaDspDriver(IPipeline& aPipeline, const Brx& aCamilloDsp, TUint aBufferUs);
};

} // namespace Media
} // namespace OpenHome

#endif // HEADER_PIPELINE_DRIVER_ALSA
