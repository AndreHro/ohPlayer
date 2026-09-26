#include "DriverAlsa.h"
#include "SoxrProcessor.h"
#include "IDataSink.h"
#include "ConvertBELE.h"
#include "OhLog.h"

#include <alsa/asoundlib.h>
#include <memory>
#include <atomic>
#include <cerrno> 


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

class DriverAlsaPimpl final : public IDriverBackend, public IDataSink
{
public:
    DriverAlsaPimpl(const Brx& aAlsaDevice, TUint aBufferUs, TUint aOutputSampleRate);
    virtual ~DriverAlsaPimpl();
    void ProcessDecodedStream(MsgDecodedStream* aMsg) override;
    void ProcessPlayable(MsgPlayable* aMsg) override;
    void ProcessDrain() override;
    void ProcessMode() override;
    void LogPCMState();
    TUint DriverDelayJiffies(TUint aSampleRate) override;
public:
    void Write(const Brx& aData) override;
    void WritePcmDirect(const Brx& aData, TUint aNumChannels, TUint aSubsampleBytes);
private:
    TBool TryProfile(Profile& aProfile, TUint aBitDepth, TUint aNumChannels,
                     TUint aSampleRate, TUint aBufferUs);
private:
    snd_pcm_t* iHandle;
    Bwh iSampleBuffer;  // buffer ProcessSampleX data
    TUint iSampleBytes;
    TBool iDuplicateChannel;
    std::vector<Profile> iProfiles;
    std::atomic<TInt> iProfileIndex{-1};
    TBool iDitch;
    TUint iBytesSent;
    TUint iBufferUs;

    TUint iOutputSampleRate;
    std::atomic<TUint> iCurrentOutputRate{0};

    std::vector<TInt32> iTmpOut;
    std::vector<TByte>  iS32ConversionBuffer;

    static const TUint kSampleBufSize = 16 * 1024;
};

DriverAlsaPimpl::DriverAlsaPimpl(const Brx& aAlsaDevice, TUint aBufferUs, TUint aOutputSampleRate)
: iHandle(nullptr)
, iSampleBuffer(kSampleBufSize)
, iSampleBytes(0)
, iDuplicateChannel(false)
, iProfileIndex(-1)
, iDitch(false)
, iBytesSent(0)
, iBufferUs(aBufferUs)
, iOutputSampleRate(aOutputSampleRate)
, iCurrentOutputRate(aOutputSampleRate)
{
    const Brhz devName(aAlsaDevice);
    const int err = snd_pcm_open(&iHandle, devName.CString(), SND_PCM_STREAM_PLAYBACK, 0);

    if (err < 0) {
        OhLog::PrintError(
            "DriverAlsa: snd_pcm_open() failed: %s\n", snd_strerror(err));
        iHandle = nullptr;
        return;
    }

    AudioSpec initialSpec{};
    initialSpec.iNumChannels = 2;
    initialSpec.iBitDepth = 32;
    initialSpec.iInputRate = 44100.0;
    initialSpec.iOutputRate = aOutputSampleRate != 0 ? aOutputSampleRate : 44100.0;

    iProfiles.emplace_back(
        new SoxrPcmProcessor(*this, iSampleBuffer, initialSpec),
        OutputFormat(SND_PCM_FORMAT_S32_LE, 4),
        OutputFormat(SND_PCM_FORMAT_S32_LE, 4),
        OutputFormat(SND_PCM_FORMAT_S32_LE, 4),
        OutputFormat(SND_PCM_FORMAT_S32_LE, 4));
}

DriverAlsaPimpl::~DriverAlsaPimpl()
{
    if (iHandle != nullptr) {
        const int err = snd_pcm_close(iHandle);
        ASSERT(err == 0);
        iHandle = nullptr;
    }
}

void DriverAlsaPimpl::ProcessPlayable(MsgPlayable* aMsg)
{
    const TInt profileIndex = iProfileIndex.load();
    const TUint index=static_cast<TUint>(profileIndex);
    if (profileIndex < 0 || (index >= iProfiles.size()) || iDitch) {
        return;
    }
    aMsg->Read(iProfiles[index].GetPcmProcessor());
}

void DriverAlsaPimpl::ProcessDrain()
{
    const TInt profileIndex = iProfileIndex.load();
    if (profileIndex == -1) {
        return;
    }

    // EndBlock flushes the resampler before ALSA is drained.
    iProfiles[profileIndex].GetPcmProcessor().EndBlock();

    auto err = snd_pcm_drain(iHandle);
    if (err < 0) {
        OhLog::PrintError("DriverAlsa: snd_pcm_drain() error: %s\n",
                          snd_strerror(err));
        ASSERTS();
        return;
    }

    err = snd_pcm_prepare(iHandle);
    if (err < 0) {
        OhLog::PrintError("DriverAlsa: snd_pcm_prepare() error: %s\n",
                          snd_strerror(err));
        ASSERTS();
    }
}

void DriverAlsaPimpl::Write(const Brx& aData)
{
    if (iSampleBytes == 0) {
        OhLog::PrintError("DriverAlsa: invalid sample size\n");
        return;
    }

    const TByte* ptr = aData.Ptr();
    snd_pcm_uframes_t framesRemaining = aData.Bytes() / iSampleBytes;
    
    TUint recoveryAttempts = 0;
    TUint retryTimeouts=0;
    const TUint kMaxRecoveryAttempts = 3; // Prevent infinite spinning loops

    while (framesRemaining > 0) {
        snd_pcm_sframes_t framesWritten = snd_pcm_writei(iHandle, ptr, framesRemaining);

        // 1. Handle Errors Gracefully
        if (framesWritten < 0) {
            if (framesWritten == -ENODEV || framesWritten == -ESHUTDOWN) {
                OhLog::PrintError("DriverAlsa: Sound card disconnected. Dropping frame payload.\n");
                return; 
            }

            // Log if it's a standard buffer underrun (Xrun)
            if (framesWritten == -EPIPE) {
                OhLog::PrintWarning("DriverAlsa: Buffer underrun (Xrun) detected. Attempting recovery...\n");
            }

            // Attempt to restore the hardware state
            const int err = snd_pcm_recover(iHandle, framesWritten, 1);
            if (err < 0) {
                OhLog::PrintError("DriverAlsa: snd_pcm_recover failed: %s\n", snd_strerror(err));
                return; // Break out immediately to protect the pipeline thread
            }

            // Protect against infinite loop lockups if recovery keeps looping
            recoveryAttempts++;
            if (recoveryAttempts > kMaxRecoveryAttempts) {
                OhLog::PrintError("DriverAlsa: Unrecoverable recovery loop cascade. Dropping block.\n");
                return;
            }

            continue;
        }
        if (framesWritten == 0) {
            const int err = snd_pcm_wait(iHandle, 50);

            if (err < 0) {
                OhLog::PrintError(
                    "DriverAlsa: snd_pcm_wait() error: %s\n",
                    snd_strerror(err));
                return;
            }

            if (err == 0) {
                ++retryTimeouts;

                if (retryTimeouts > kMaxRecoveryAttempts) {
                    OhLog::PrintError(
                        "DriverAlsa: Repeated ALSA wait timeouts. "
                        "Dropping block.\n");
                    return;
                }
            }
            else {
                retryTimeouts = 0;
            }

            continue;
        }
    }
}

void DriverAlsaPimpl::WritePcmDirect(const Brx& aData, TUint aNumChannels, TUint aSubsampleBytes)
{
    if (iSampleBytes == 0) {
        OhLog::PrintError("DriverAlsa: invalid sample size\n");
        return;
    }

    const TUint inputBytes = aData.Bytes();
    const TUint bytesPerFrame = aNumChannels * aSubsampleBytes;
    const TUint frames = inputBytes / bytesPerFrame;
    if (frames == 0) {
        return;
    }

    const TUint totalSamples = frames * aNumChannels;
    const TUint outputBytes = totalSamples * sizeof(TInt32);

    if (iS32ConversionBuffer.size() < outputBytes) {
        iS32ConversionBuffer.resize(outputBytes);
    }

    OpenHome::Media::ConvertInterleavedToS32LE_With24Simd(
        aData.Ptr(),
        inputBytes,
        iS32ConversionBuffer.data(),
        frames,
        aNumChannels,
        aSubsampleBytes,
        false /* OH PIPELINE IS ALWAYS BE */,
        iDuplicateChannel && aNumChannels == 1,
        iTmpOut);

    const Brn converted(iS32ConversionBuffer.data(), outputBytes);
    Write(converted);
}

#ifdef DEBUG
void DriverAlsaPimpl::LogPCMState()
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

void DriverAlsaPimpl::ProcessDecodedStream(MsgDecodedStream* aMsg)
{
    const TInt profileIndex = iProfileIndex.load();
    if (profileIndex != -1) {
        ProcessDrain();
    }

    const auto & decodedStreamInfo = aMsg->StreamInfo();

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

    const TUint outputRate = iOutputSampleRate ? iOutputSampleRate
                                                : decodedStreamInfo.SampleRate();
    iCurrentOutputRate.store(outputRate);

    for (TUint i = 0; i < iProfiles.size(); ++i)
    {
        if (TryProfile(iProfiles[i], decodedStreamInfo.BitDepth(),
                    decodedStreamInfo.NumChannels(),
                    outputRate, iBufferUs))
        {
            iProfileIndex.store(i);
            SoxrPcmProcessor& pcmProcessor =
                    static_cast<SoxrPcmProcessor&>(
                        iProfiles[i].GetPcmProcessor());

            AudioSpec spec{};
            spec.iNumChannels = decodedStreamInfo.NumChannels();
            spec.iBitDepth = decodedStreamInfo.BitDepth();
            spec.iInputRate = decodedStreamInfo.SampleRate();
            spec.iOutputRate = outputRate;

            pcmProcessor.SetDuplicateChannel(iDuplicateChannel);
            pcmProcessor.UpdateFormatSpec(spec);

            const OutputFormat outputFormat =
            iProfiles[i].GetFormat(spec.iBitDepth);

            const TUint outputChannels =
                (iDuplicateChannel && spec.iNumChannels == 1) ? 2 :
                spec.iNumChannels;

            iSampleBytes = outputChannels * outputFormat.second;
            iDitch = false;

            Log::Print("Found PcmProcessor %d\n", i);

            return;
        }
    }

    OhLog::PrintWarning("DriverAlsa: Could not find a PcmProcessor for stream! "
               "BitDepth = %d, SampleRate = %d, Channels = %d\n",
               decodedStreamInfo.BitDepth(), decodedStreamInfo.SampleRate(),
               decodedStreamInfo.NumChannels());

    iDitch = true;
    iProfileIndex.store(-1);
}

TBool DriverAlsaPimpl::TryProfile(Profile& aProfile,
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

    if (iHandle == nullptr) {
        return false;
    }

    int err = snd_pcm_hw_free(iHandle);
    if (err < 0) {
        OhLog::PrintError(
            "DriverAlsa: snd_pcm_hw_free() failed: %s\n",
            snd_strerror(err));
        return false;
    }

    err = snd_pcm_hw_params_any(iHandle, hwParams);
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

TUint DriverAlsaPimpl::DriverDelayJiffies(TUint aSampleRate)
{
    const TUint rate = iCurrentOutputRate.load() != 0
        ? iCurrentOutputRate.load()
        : aSampleRate;

    if (!aSampleRate || iProfileIndex.load() == -1) {
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

    return (TUint)delayFrames * Jiffies::PerSample(rate);
}

void DriverAlsaPimpl::ProcessMode()
{
    const TInt profileIndex = iProfileIndex.load();

    Log::Print(
        "DriverAlsa: MsgMode intercepted. Purging pipeline tracking state.\n");

    if (profileIndex >= 0 &&
        static_cast<size_t>(profileIndex) < iProfiles.size()) {
        iProfiles[static_cast<size_t>(profileIndex)]
            .GetPcmProcessor()
            .Flush();
    }

    iSampleBytes = 0;
    iDitch = false;
    iBytesSent = 0;
    iProfileIndex.store(-1);
}

DriverAlsa::DriverAlsa(IPipeline& aPipeline, const Brx& aAlsaDevice, TUint aBufferUs, TUint aOutputSampleRate)
: PipelineDriverBase(aPipeline, std::make_unique<DriverAlsaPimpl>(aAlsaDevice, aBufferUs, aOutputSampleRate))
{}

PipelineDriverBase::PipelineDriverBase(
    IPipeline& aPipeline,
    std::unique_ptr<IDriverBackend> aBackend)
    : PipelineElement(GetSupportedElements())
    , iPipeline(aPipeline)
    , iMutex("PipelineDriverBase")
    , iQuit(false)
    , iBackend(std::move(aBackend))
    , iThread(nullptr)
{
    iPipeline.SetAnimator(*this);
    iThread = std::make_unique<ThreadFunctor>(
    "PipelineAnimator",
    MakeFunctor(*this, &PipelineDriverBase::AudioThread),
    kPrioritySystemHighest);
    iThread->Start();
}

PipelineDriverBase::~PipelineDriverBase()
{}

void PipelineDriverBase::PipelineAnimatorGetMaxSampleRates(TUint& aPcm, TUint& aDsd) const
{
    aPcm = 192000;
    aDsd = 5644800;
}

void PipelineDriverBase::AudioThread()
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

TUint PipelineDriverBase::PipelineAnimatorBufferJiffies() const
{
	return 0;
}

TUint PipelineDriverBase::PipelineAnimatorDelayJiffies(AudioFormat aFormat,
											   TUint aSampleRate,
                                               TUint /*aBitDepth*/,
                                               TUint /*aNumChannels*/) const
{
	if (aFormat == AudioFormat::Dsd) {
		THROW(FormatUnsupported);
	}
    return iBackend->DriverDelayJiffies(aSampleRate);
}

void PipelineDriverBase::PipelineAnimatorDsdBlockConfiguration(TUint& aSampleBlockWords, TUint& aPadBytesPerChunk) const
{
    (void)aSampleBlockWords;
    (void)aPadBytesPerChunk;
}

TUint PipelineDriverBase::PipelineAnimatorMaxBitDepth() const
{
    return 0;
}

Msg* PipelineDriverBase::ProcessMsg(MsgHalt* aMsg)
{
    aMsg->ReportHalted();

    return aMsg;
}

Msg* PipelineDriverBase::ProcessMsg(MsgDecodedStream* aMsg)
{
    iBackend->ProcessDecodedStream(aMsg);

    return aMsg;
}

Msg* PipelineDriverBase::ProcessMsg(MsgPlayable* aMsg)
{
    iBackend->ProcessPlayable(aMsg);

    return aMsg;
}

Msg* PipelineDriverBase::ProcessMsg(MsgQuit* aMsg)
{
    AutoMutex am(iMutex);
    iQuit = true;
    return aMsg;
}

Msg* PipelineDriverBase::ProcessMsg(MsgMode* aMsg)
{
    const Brx& modeName = aMsg->Mode();
    Log::Print("DriverAlsa: MsgMode intercepted. %s \n", PBUF(modeName));

    iBackend->ProcessMode();

    return aMsg;
}

Msg* PipelineDriverBase::ProcessMsg(MsgDrain* aMsg)
{
    // Ensure the ALSA audio buffer is emptied.
    iBackend->ProcessDrain();
    aMsg->ReportDrained();

    return aMsg;
}
