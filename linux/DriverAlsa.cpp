#include "DriverAlsa.h"
#include "SoxrProcessor.h"
#include "IDataSink.h"
#include "OhLog.h"

#include <alsa/asoundlib.h>
#include <memory>


using namespace OpenHome;
using namespace OpenHome::Media;


PriorityArbitratorDriver::PriorityArbitratorDriver(TUint aOpenHomeMax)
: iOpenHomeMax(aOpenHomeMax)
{
}

TUint PriorityArbitratorDriver::Priority(const TChar* /*aId*/, TUint aRequested, TUint aHostMax)
{
    ASSERT(aRequested == iOpenHomeMax);
    return aHostMax;
}

TUint PriorityArbitratorDriver::OpenHomeMin() const
{
    return iOpenHomeMax;
}

TUint PriorityArbitratorDriver::OpenHomeMax() const
{
    return iOpenHomeMax;
}

TUint PriorityArbitratorDriver::HostRange() const
{
    return 1;
}

using OutputFormat=std::pair<snd_pcm_format_t, TUint>;

class Profile
{
public:
    Profile(IPcmProcessor* aPcmProcessor, OutputFormat aFormat32,
                                          OutputFormat aFormat24,
                                          OutputFormat aFormat16,
                                          OutputFormat aFormat8);
public:
    OutputFormat   GetFormat(TUint aBitDepth) const;
    IPcmProcessor& GetPcmProcessor() const;
private:
    std::unique_ptr<IPcmProcessor> iPcmProcessor;
    OutputFormat                   iOutputDesc[4];
};

Profile::Profile(IPcmProcessor* aPcmProcessor, OutputFormat aFormat32,
                                               OutputFormat aFormat24,
                                               OutputFormat aFormat16,
                                               OutputFormat aFormat8)
: iPcmProcessor(aPcmProcessor)
{
    iOutputDesc[0] = aFormat32;
    iOutputDesc[1] = aFormat24;
    iOutputDesc[2] = aFormat16;
    iOutputDesc[3] = aFormat8;
}

OutputFormat Profile::GetFormat(TUint aBitDepth) const
{
    switch (aBitDepth)
    {
        case 32:
            return iOutputDesc[0];
        case 24:
            return iOutputDesc[1];
        case 16:
            return iOutputDesc[2];
        case 8:
            return iOutputDesc[3];
        default:
            ASSERTS();
            return iOutputDesc[0];
    }
}

IPcmProcessor& Profile::GetPcmProcessor() const
{
    return *iPcmProcessor;
}

/*  Pimpl

    Private implementation of ALSA output. Takes MsgPlayable
    and plays it.
*/

class DriverAlsa::Pimpl : public IDataSink
{
public:
    Pimpl(const TChar* aAlsaDevice, TUint aBufferUs);
    virtual ~Pimpl();
    void ProcessDecodedStream(MsgDecodedStream* aMsg);
    void ProcessPlayable(MsgPlayable* aMsg);
    void ProcessDrain();
    void LogPCMState();
    TUint DriverDelayJiffies(TUint aSampleRate);
public:
    virtual void Write(const Brx& aData);
private:
    TBool TryProfile(Profile& aProfile, TUint aBitDepth, TUint aNumChannels,
                     TUint aSampleRate, TUint aBufferUs);
private:
    snd_pcm_t* iHandle;
    Bwh iSampleBuffer;  // buffer ProcessSampleX data
    TUint iSampleBytes;
    TBool iDuplicateChannel;
    std::vector<Profile> iProfiles;
    TInt iProfileIndex;
    TBool iDitch;
    TUint iBytesSent;
    TUint iBufferUs;

    static const TUint kSampleBufSize = 16 * 1024;
};

DriverAlsa::Pimpl::Pimpl(const TChar* aAlsaDevice, TUint aBufferUs)
: iHandle(nullptr)
, iSampleBuffer(kSampleBufSize)
, iSampleBytes(0)
, iDuplicateChannel(false)
, iProfileIndex(-1)
, iDitch(false)
, iBytesSent(0)
, iBufferUs(aBufferUs)
{
    auto err = snd_pcm_open(&iHandle, aAlsaDevice, SND_PCM_STREAM_PLAYBACK, 0);
    ASSERT(err == 0);
    AudioSpec initialSpec{};
    initialSpec.iSampleBytes = 4;
    initialSpec.iNumChannels = 2;
    initialSpec.iBitDepth = 32;
    initialSpec.iInputRate = 44100.0;
    initialSpec.iOutputRate = 44100.0;

    iProfiles.emplace_back(
        new SoxrPcmProcessor(*this, iSampleBuffer, initialSpec),
        OutputFormat(SND_PCM_FORMAT_S32_LE, 4),
        OutputFormat(SND_PCM_FORMAT_S32_LE, 4),
        OutputFormat(SND_PCM_FORMAT_S32_LE, 4),
        OutputFormat(SND_PCM_FORMAT_S32_LE, 4));
}

DriverAlsa::Pimpl::~Pimpl()
{
    auto err = snd_pcm_close(iHandle);
    ASSERT(err == 0);
}

void DriverAlsa::Pimpl::ProcessPlayable(MsgPlayable* aMsg)
{
    if (! iDitch)
    	aMsg->Read(iProfiles[iProfileIndex].GetPcmProcessor());
}

void DriverAlsa::Pimpl::ProcessDrain()
{
    // Wait for the native audio buffers to empty.
    if (iProfileIndex != -1)
    {
        iProfiles[iProfileIndex].GetPcmProcessor().EndBlock();
        auto err = snd_pcm_drain(iHandle);
        if (err < 0)
        {
            OhLog::PrintError("DriverAlsa: snd_pcm_drain() error : %s\n",
                       snd_strerror(err));
            ASSERTS();
        }

        // Prepare the PCM to accept new data.
        err = snd_pcm_prepare(iHandle);

        if (err < 0)
        {
            OhLog::PrintError("DriverAlsa: snd_pcm_prepare() error : %s\n",
                       snd_strerror(err));
            ASSERTS();
        }
    }
}
#if 1
void DriverAlsa::Pimpl::Write(const Brx& aData)
{
    if (iSampleBytes == 0) {
        OhLog::PrintError("DriverAlsa: invalid sample size\n");
        return;
    }

    const TByte* ptr = aData.Ptr();
    snd_pcm_uframes_t framesRemaining = aData.Bytes() / iSampleBytes;

    while (framesRemaining > 0) {
        snd_pcm_sframes_t framesWritten =
            snd_pcm_writei(iHandle, ptr, framesRemaining);

        if (framesWritten < 0) {
            const int err = snd_pcm_recover(iHandle, framesWritten, 1);

            if (err < 0) {
                OhLog::PrintError("DriverAlsa: snd_pcm_writei() unrecoverable error: %s\n",
                                  snd_strerror(err));
                return;
            }

            continue;
        }

        if (framesWritten == 0) {
            const int err = snd_pcm_wait(iHandle, 1000);

            if (err < 0) {
                OhLog::PrintError("DriverAlsa: snd_pcm_wait() error: %s\n",
                                  snd_strerror(err));
                return;
            }

            continue;
        }

        ptr += framesWritten * iSampleBytes;
        framesRemaining -= framesWritten;
        iBytesSent += framesWritten * iSampleBytes;
    }
}
#else
void DriverAlsa::Pimpl::Write(const Brx& aData)
{
    int err;

    err = snd_pcm_writei(iHandle, aData.Ptr(), aData.Bytes() / iSampleBytes);

    // Handle underrun errors.
    if(err == -EPIPE) {
        err = snd_pcm_prepare(iHandle);

        if (err < 0)
        {
            OhLog::PrintError("DriverAlsa: failed to snd_pcm_recover with %s\n",
                       snd_strerror(err));
            ASSERTS();
        }

        err = snd_pcm_writei(iHandle,
                             aData.Ptr(),
                             aData.Bytes() / iSampleBytes);
    }


    if (err < 0)
    {
        OhLog::PrintError("DriverAlsa: snd_pcm_writei() got error %s\n",
                   snd_strerror(err));
    }
    else
    {
        iBytesSent += aData.Bytes();
    }
}
#endif
#ifdef DEBUG
void DriverAlsa::Pimpl::LogPCMState()
{
    switch (snd_pcm_state(iHandle))
    {
        case SND_PCM_STATE_OPEN:
            Log::Print("PCM STATE: SND_PCM_STATE_OPEN\n");
            break;
        case SND_PCM_STATE_SETUP:
            Log::Print("PCM STATE: SND_PCM_STATE_SETUP\n");
            break;
        case SND_PCM_STATE_PREPARED:
            Log::Print("PCM STATE: SND_PCM_STATE_PREPARED\n");
            break;
        case SND_PCM_STATE_RUNNING:
            Log::Print("PCM STATE: SND_PCM_STATE_RUNNING\n");
            break;
        case SND_PCM_STATE_XRUN:
            Log::Print("PCM STATE: SND_PCM_STATE_XRUN\n");
            break;
        case SND_PCM_STATE_DRAINING:
            Log::Print("PCM STATE: SND_PCM_STATE_DRAINING\n");
            break;
        case SND_PCM_STATE_PAUSED:
            Log::Print("PCM STATE: SND_PCM_STATE_PAUSED\n");
            break;
        case SND_PCM_STATE_SUSPENDED:
            Log::Print("PCM STATE: SND_PCM_STATE_SUSPENDED\n");
            break;
        case SND_PCM_STATE_DISCONNECTED:
            Log::Print("PCM STATE: SND_PCM_STATE_DISCONNECTED\n");
            break;
        default:
            Log::Print("PCM STATE: UNKNOWN\n");
            break;
    }
}
#endif

void DriverAlsa::Pimpl::ProcessDecodedStream(MsgDecodedStream* aMsg)
{
    if (iProfileIndex != -1)
    {
        // Drain and stop the PCM.
        auto err = snd_pcm_drain(iHandle);
        if (err < 0)
        {
            OhLog::PrintError("DriverAlsa: snd_pcm_drain() error : %s\n",
                       snd_strerror(err));
            ASSERTS();
        }
    }

    auto decodedStreamInfo = aMsg->StreamInfo();

    Log::Print("DriverAlsa: Bytes Sent since last MsgDecodedStream = %d\n",
               iBytesSent);

    iBytesSent = 0;

    Log::Print("DriverAlsa: Finding PcmProcessor for stream: BitDepth = %d, "
               "SampleRate = %d, Channels = %d\n",
               decodedStreamInfo.BitDepth(), decodedStreamInfo.SampleRate(),
               decodedStreamInfo.NumChannels());

    // Mono plays badly on the Raspberry Pi and causes issues when
    // switching to a stereo track.
    //
    // So we configure the playback for stereo and duplicate the
    // channel data.
    if (decodedStreamInfo.NumChannels() == 1)
    {
        iDuplicateChannel = true;
    }
    else
    {
        iDuplicateChannel = false;
    }

    for (TUint i = 0; i < iProfiles.size(); ++i)
    {
        if (TryProfile(iProfiles[i], decodedStreamInfo.BitDepth(),
                       decodedStreamInfo.NumChannels(),
                       decodedStreamInfo.SampleRate(), iBufferUs))
        {
            iProfileIndex = i;
            SoxrPcmProcessor& pcmProcessor =
                    static_cast<SoxrPcmProcessor&>(
                        iProfiles[i].GetPcmProcessor());

            AudioSpec spec{};
            spec.iSampleBytes =
                decodedStreamInfo.BitDepth() == 8 ? 1 :
                decodedStreamInfo.BitDepth() == 16 ? 2 :
                decodedStreamInfo.BitDepth() == 24 ? 3 : 4;
            spec.iNumChannels = decodedStreamInfo.NumChannels();
            spec.iBitDepth = decodedStreamInfo.BitDepth();
            spec.iInputRate = decodedStreamInfo.SampleRate();
            spec.iOutputRate = decodedStreamInfo.SampleRate();

            pcmProcessor.SetDuplicateChannel(iDuplicateChannel);
            pcmProcessor.UpdateFormatSpec(spec);

            const OutputFormat outputFormat =
            iProfiles[i].GetFormat(spec.iBitDepth);

            const TUint outputChannels =
                (iDuplicateChannel && spec.iNumChannels == 1) ? 2 :
                spec.iNumChannels;

            iSampleBytes = outputChannels * outputFormat.second;
            iDitch = false;

            Log::Print("Found PcmProcessor %d\n", iProfileIndex);

            return;
        }
    }

    OhLog::PrintWarning("DriverAlsa: Could not find a PcmProcessor for stream! "
               "BitDepth = %d, SampleRate = %d, Channels = %d\n",
               decodedStreamInfo.BitDepth(), decodedStreamInfo.SampleRate(),
               decodedStreamInfo.NumChannels());

    iDitch = true;
    iProfileIndex = -1;
}
TBool DriverAlsa::Pimpl::TryProfile(Profile& aProfile,
                                    TUint aBitDepth,
                                    TUint aNumChannels,
                                    TUint aSampleRate,
                                    TUint aBufferUs)
{
    auto outputFormat = aProfile.GetFormat(aBitDepth);


    snd_pcm_hw_params_t* hwParams;
    snd_pcm_sw_params_t* swParams;
    snd_pcm_hw_params_alloca(&hwParams);
    snd_pcm_sw_params_alloca(&swParams);

    int err = snd_pcm_hw_params_any(iHandle, hwParams);
    if (err < 0) return false;

    err = snd_pcm_hw_params_set_access(iHandle, hwParams,
                                       SND_PCM_ACCESS_RW_INTERLEAVED);
    if (err < 0) return false;

    err = snd_pcm_hw_params_set_format(iHandle, hwParams,
                                       outputFormat.first);
    if (err < 0) return false;

    const TUint outputChannels =
    (iDuplicateChannel && aNumChannels == 1) ? 2 :
    aNumChannels;

    err = snd_pcm_hw_params_set_channels(
        iHandle,
        hwParams,
        outputChannels);
    if (err < 0) {
        return false;
    }

    unsigned int rate = aSampleRate;
    err = snd_pcm_hw_params_set_rate_near(iHandle, hwParams, &rate, nullptr);
    if (err < 0 || rate != aSampleRate) return false;

    unsigned int bufferTime = aBufferUs;
    err = snd_pcm_hw_params_set_buffer_time_near(iHandle,
                                                 hwParams,
                                                 &bufferTime,
                                                 nullptr);
    if (err < 0) return false;

    // Réglage plus adapté SPI / embarqué :
    // plusieurs petites périodes dans un buffer assez confortable.
    unsigned int periodTime = bufferTime / 8;
    if (periodTime < 10000) {
        periodTime = 10000; // 10 ms minimum
    }

    err = snd_pcm_hw_params_set_period_time_near(iHandle,
                                                 hwParams,
                                                 &periodTime,
                                                 nullptr);
    if (err < 0) return false;

    err = snd_pcm_hw_params(iHandle, hwParams);
    if (err < 0) return false;

    snd_pcm_uframes_t bufferSize = 0;
    snd_pcm_uframes_t periodSize = 0;

    snd_pcm_hw_params_get_buffer_size(hwParams, &bufferSize);
    snd_pcm_hw_params_get_period_size(hwParams, &periodSize, nullptr);

    err = snd_pcm_sw_params_current(iHandle, swParams);
    if (err < 0) return false;

    err = snd_pcm_sw_params_set_start_threshold(iHandle,
                                                swParams,
                                                periodSize);
    if (err < 0) return false;

    err = snd_pcm_sw_params_set_avail_min(iHandle,
                                          swParams,
                                          periodSize);
    if (err < 0) return false;

    err = snd_pcm_sw_params(iHandle, swParams);
    if (err < 0) return false;

    err = snd_pcm_prepare(iHandle);
    if (err < 0) return false;

    Log::Print("DriverAlsa: configured ALSA: rate=%u channels=%u "
           "format=%d buffer=%lu frames period=%lu frames\n",
           rate,
           outputChannels,
           outputFormat.first,
           bufferSize,
           periodSize);

    return true;
}
TUint DriverAlsa::Pimpl::DriverDelayJiffies(TUint aSampleRate)
{
    if (!aSampleRate || iProfileIndex == -1) {
        return 0;
    }

    snd_pcm_sframes_t delayFrames = 0;
    int ret = snd_pcm_delay(iHandle, &delayFrames);

    if (ret < 0) {
        ret = snd_pcm_recover(iHandle, ret, 1);
        if (ret < 0) {
            OhLog::PrintError("DriverAlsa: snd_pcm_delay() error: %s\n",
                              snd_strerror(ret));
            return 0;
        }

        ret = snd_pcm_delay(iHandle, &delayFrames);
        if (ret < 0) {
            return 0;
        }
    }

    if (delayFrames < 0) {
        delayFrames = 0;
    }

    return (TUint)delayFrames * Jiffies::PerSample(aSampleRate);
}

// DriverAlsa

const TUint DriverAlsa::kSupportedMsgTypes = PipelineElement::MsgType::eMode
| PipelineElement::MsgType::eDrain
| PipelineElement::MsgType::eHalt
| PipelineElement::MsgType::eDecodedStream
| PipelineElement::MsgType::ePlayable
| PipelineElement::MsgType::eQuit;

DriverAlsa::DriverAlsa(IPipeline& aPipeline, TUint aBufferUs)
    : PipelineElement(kSupportedMsgTypes)
    , iPimpl(new Pimpl("default", aBufferUs))
    , iPipeline(aPipeline)
    , iMutex("alsa")
    , iQuit(false)
{
    iPipeline.SetAnimator(*this);

    iThread = new ThreadFunctor("PipelineAnimator",
                                MakeFunctor(*this, &DriverAlsa::AudioThread),
                                kPrioritySystemHighest);
    iThread->Start();
}

DriverAlsa::~DriverAlsa()
{
    delete iThread;
    delete iPimpl;
}
void DriverAlsa::PipelineAnimatorGetMaxSampleRates(TUint& aPcm, TUint& aDsd) const
{
    aPcm = 192000;
    aDsd = 5644800;
}
void DriverAlsa::AudioThread()
{
    try
    {
        for (;;)
        {
            Msg* msg = iPipeline.Pull();
            msg = msg->Process(*this);
            if (msg != NULL)
            {
                msg->RemoveRef();
            }

            AutoMutex am(iMutex);
            if (iQuit)
                break;
        }
    }
    catch (ThreadKill&) {}
}

TUint DriverAlsa::PipelineAnimatorBufferJiffies() const
{
	return 0;
}

TUint DriverAlsa::PipelineAnimatorDelayJiffies(AudioFormat aFormat,
											   TUint aSampleRate,
                                               TUint /*aBitDepth*/,
                                               TUint /*aNumChannels*/) const
{
	if (aFormat == AudioFormat::Dsd) {
		THROW(FormatUnsupported);
	}
    return iPimpl->DriverDelayJiffies(aSampleRate);
}

void DriverAlsa::PipelineAnimatorDsdBlockConfiguration(TUint& aSampleBlockWords, TUint& aPadBytesPerChunk) const
{
}

TUint DriverAlsa::PipelineAnimatorMaxBitDepth() const
{
    return 0;
}

Msg* DriverAlsa::ProcessMsg(MsgHalt* aMsg)
{
    aMsg->ReportHalted();

    return aMsg;
}

Msg* DriverAlsa::ProcessMsg(MsgDecodedStream* aMsg)
{
    iPimpl->ProcessDecodedStream(aMsg);
    return aMsg;
}

Msg* DriverAlsa::ProcessMsg(MsgPlayable* aMsg)
{
    iPimpl->ProcessPlayable(aMsg);
    return aMsg;
}

Msg* DriverAlsa::ProcessMsg(MsgQuit* aMsg)
{
    AutoMutex am(iMutex);
    iQuit = true;
    return aMsg;
}

Msg* DriverAlsa::ProcessMsg(MsgMode* aMsg)
{
    // TODO
    return aMsg;
}

Msg* DriverAlsa::ProcessMsg(MsgDrain* aMsg)
{
    // Ensure the ALSA audio buffer is emptied.
    iPimpl->ProcessDrain();

    aMsg->ReportDrained();

    return aMsg;
}
