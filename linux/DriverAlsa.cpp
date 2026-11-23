#include "DriverAlsa.h"
#include "KfrPcmProcessor.h"
#include "IDataSink.h"
#include "ConvertBELE.h"
#include "OhLog.h"
#include <alsa/asoundlib.h>
#include <errno.h>
#include <memory>
#include <atomic>
#include <algorithm>
#include <cerrno> 
#include <cstdint>
#include <fstream>
#include <limits>
#include <string>
#include <chrono>
#include <mutex>


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
    Profile(std::unique_ptr<IPcmProcessor> aPcmProcessor, OutputFormat aFormat32,
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

Profile::Profile(std::unique_ptr<IPcmProcessor> aPcmProcessor, OutputFormat aFormat32,
                                                               OutputFormat aFormat24,
                                                               OutputFormat aFormat16,
                                                               OutputFormat aFormat8)
: iPcmProcessor(std::move(aPcmProcessor))
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
    assert(iPcmProcessor != nullptr);
    return *iPcmProcessor;
}

/*  Pimpl

    Private implementation of ALSA output. Takes MsgPlayable
    and plays it.
*/

class DriverAlsaPimpl final : public IDriverBackend, public IDataSink
{
public:
    DriverAlsaPimpl(const Brx& aAlsaDevice, 
        TUint aBufferUs, TUint aOutputSampleRate,
        const Brx& aKfrIrConfigDir);
    virtual ~DriverAlsaPimpl();
    void ProcessDecodedStream(MsgDecodedStream* aMsg) override;
    void ProcessPlayable(MsgPlayable* aMsg) override;
    void ProcessDrain() override;
    void ProcessMode(const Brx& aMode) override;
    void LogPCMState();
    TUint DriverDelayJiffies(TUint aSampleRate) override;
public:
    void Write(const Brx& aData) override;
#if 0    
    void WritePcmDirect(const Brx& aData, TUint aNumChannels, TUint aSubsampleBytes);
#endif

private:
    static TBool IsDeviceDisconnected(int aError);
    TBool EnsureDeviceOpen();
    TBool IsAlsaCardPresent() const;
    void CloseDevice();
    TBool ReconfigureCurrentStreamLocked();

private:
    TBool TryProfileLocked(Profile& aProfile, TUint aBitDepth, TUint aNumChannels,
                           TUint aSampleRate, TUint aBufferUs);
private:
    snd_pcm_t* iHandle{nullptr};
    Bwh iSampleBuffer;  // buffer ProcessSampleX data
    TUint iSampleBytes;
    TBool iDuplicateChannel;
    std::unique_ptr<Profile> iProfile;
    TBool iDitch;
    TUint iBytesSent;
    TUint iXrunCount;
    TUint iRecoverCount;
    TUint iWaitCount;
    TUint iBufferUs;
    std::string iKfrIrConfigDir;

    std::string iAlsaDevice;
    mutable std::mutex iAlsaMutex;
    std::chrono::steady_clock::time_point iLastOpenAttempt{};
    TBool iLastOpenFailedLogged{false};

    TUint iLastBitDepth{0};
    TUint iLastNumChannels{0};
    TUint iLastInputRate{0};
    TBool iHasStreamSpec{false};

    TUint iOutputSampleRate;
    std::atomic<TUint> iCurrentOutputRate{0};
    TBool iLowLatencyMode{false};

    std::vector<TInt32> iTmpOut;
    std::vector<TByte>  iS32ConversionBuffer;

    static const TUint kSampleBufSize = 16 * 1024;
};

DriverAlsaPimpl::DriverAlsaPimpl(
    const Brx& aAlsaDevice,
    TUint aBufferUs,
    TUint aOutputSampleRate,
    const Brx& aKfrIrConfigDir)
: iAlsaDevice(reinterpret_cast<const char*>(aAlsaDevice.Ptr()), aAlsaDevice.Bytes())
, iHandle(nullptr)
, iSampleBuffer(kSampleBufSize)
, iSampleBytes(0)
, iDuplicateChannel(false)
, iProfile(nullptr)
, iDitch(false)
, iBytesSent(0)
, iXrunCount(0)
, iRecoverCount(0)
, iWaitCount(0)
, iBufferUs(aBufferUs)
, iKfrIrConfigDir(aKfrIrConfigDir.Bytes() == 0
    ? std::string()
    : std::string(reinterpret_cast<const char*>(aKfrIrConfigDir.Ptr()), aKfrIrConfigDir.Bytes()))
, iOutputSampleRate(aOutputSampleRate)
, iCurrentOutputRate(aOutputSampleRate)
{
    AudioSpec initialSpec{};
    initialSpec.iNumChannels = 2;
    initialSpec.iBitDepth = 32;
    initialSpec.iInputRate = 44100.0;
    initialSpec.iOutputRate = aOutputSampleRate != 0 ? aOutputSampleRate : 44100.0;

    auto pcmProcessor = std::make_unique<KfrPcmProcessor>(
        *this, iSampleBuffer, initialSpec, iKfrIrConfigDir);
    iProfile = std::make_unique<Profile>(
        std::move(pcmProcessor),
        OutputFormat(SND_PCM_FORMAT_S32_LE, 4),
        OutputFormat(SND_PCM_FORMAT_S32_LE, 4),
        OutputFormat(SND_PCM_FORMAT_S32_LE, 4),
        OutputFormat(SND_PCM_FORMAT_S32_LE, 4));

    std::lock_guard<std::mutex> lock(iAlsaMutex);
    if (!EnsureDeviceOpen()) {
        iDitch = true;
    }  
}

DriverAlsaPimpl::~DriverAlsaPimpl()
{
    std::lock_guard<std::mutex> lock(iAlsaMutex);
    CloseDevice();
}

TBool DriverAlsaPimpl::IsDeviceDisconnected(int aError)
{
    return aError == -ENODEV || aError == -ESHUTDOWN ||
           aError == -EIO || aError == -ESTRPIPE;
}

void DriverAlsaPimpl::CloseDevice()
{
    if (iHandle != nullptr) {
        snd_pcm_close(iHandle);
        iHandle = nullptr;
    }
}

TBool DriverAlsaPimpl::IsAlsaCardPresent() const
{
    const auto cardKey = iAlsaDevice.find("CARD=");
    if (cardKey == std::string::npos) {
        return true;
    }

    const auto cardStart = cardKey + sizeof("CARD=") - 1;
    const auto cardEnd = iAlsaDevice.find(',', cardStart);
    const std::string cardName = iAlsaDevice.substr(
        cardStart, cardEnd == std::string::npos ? std::string::npos : cardEnd - cardStart);
    if (cardName.empty()) {
        return true;
    }

    std::ifstream cardsFile("/proc/asound/cards");
    if (!cardsFile) {
        return true;
    }

    std::string line;
    while (std::getline(cardsFile, line)) {
        const auto idStart = line.find('[');
        const auto idEnd = line.find(']', idStart);
        if (idStart == std::string::npos || idEnd == std::string::npos) {
            continue;
        }

        const auto nameStart = line.find_first_not_of(" \t", idStart + 1);
        const auto nameEnd = line.find_last_not_of(" \t", idEnd - 1);
        if (nameStart != std::string::npos && nameEnd >= nameStart &&
            line.compare(nameStart, nameEnd - nameStart + 1, cardName) == 0 &&
            nameEnd - nameStart + 1 == cardName.size()) {
            return true;
        }
    }

    return false;
}

TBool DriverAlsaPimpl::EnsureDeviceOpen()
{
    if (iHandle != nullptr) {
        return true;
    }

    const auto now = std::chrono::steady_clock::now();
    if (iLastOpenAttempt.time_since_epoch().count() != 0 &&
        (now - iLastOpenAttempt) < std::chrono::milliseconds(500)) {
        return false;
    }
    iLastOpenAttempt = now;

    if (!IsAlsaCardPresent()) {
        if (!iLastOpenFailedLogged) {
            OhLog::PrintError("DriverAlsa: ALSA card for %s is not present; waiting for DAC.\n",
                              iAlsaDevice.c_str());
        }
        iLastOpenFailedLogged = true;
        return false;
    }

    OhLog::Print("DriverAlsa: Opening ALSA device %s\n", iAlsaDevice.c_str());
    const int err = snd_pcm_open(&iHandle, iAlsaDevice.c_str(), SND_PCM_STREAM_PLAYBACK, 0);
    if (err < 0) {
        if (!iLastOpenFailedLogged) {
            OhLog::PrintError("DriverAlsa: snd_pcm_open() failed: %s\n", snd_strerror(err));
        }
        iHandle = nullptr;
        iLastOpenFailedLogged = true;
        return false;
    }

    iLastOpenFailedLogged = false;
    OhLog::Print("DriverAlsa: ALSA device %s opened.\n", iAlsaDevice.c_str());
    return true;
}

TBool DriverAlsaPimpl::ReconfigureCurrentStreamLocked()
{
    if (!iHasStreamSpec || iProfile == nullptr) {
        return false;
    }

    iDuplicateChannel = (iLastNumChannels == 1);
    const TUint outputRate = iOutputSampleRate ? iOutputSampleRate : iLastInputRate;
    iCurrentOutputRate.store(outputRate);

    if (!TryProfileLocked(*iProfile, iLastBitDepth, iLastNumChannels,
                          outputRate, iBufferUs)) {
        iSampleBytes = 0;
        iDitch = true;
        return false;
    }

    const OutputFormat outputFormat = iProfile->GetFormat(iLastBitDepth);
    const TUint outputChannels =
        (iDuplicateChannel && iLastNumChannels == 1) ? 2 : iLastNumChannels;
    iSampleBytes = outputChannels * outputFormat.second;
    iDitch = false;
    return true;
}

void DriverAlsaPimpl::ProcessPlayable(MsgPlayable* aMsg)
{
    if (iProfile == nullptr || (iDitch && !iHasStreamSpec)) {
        return;
    }
    auto& pcmProcessor = iProfile->GetPcmProcessor();
    aMsg->Read(pcmProcessor);
}

void DriverAlsaPimpl::ProcessDrain()
{
    if (iProfile == nullptr) {
        return;
    }
    // Flush the resampler before ALSA is drained.
    iProfile->GetPcmProcessor().Flush();

    std::lock_guard<std::mutex> lock(iAlsaMutex);
    if (iHandle == nullptr) {
        return;
    }
    snd_pcm_state_t state = snd_pcm_state(iHandle);
    if (state == SND_PCM_STATE_DISCONNECTED || static_cast<int>(state) < 0) {
        CloseDevice();
        return;
    }
    if (state == SND_PCM_STATE_XRUN) {
        const int recoverError = snd_pcm_recover(iHandle, -EPIPE, 1);
        if (recoverError < 0) {
            OhLog::PrintError("DriverAlsa: XRUN recovery failed: %s\n",
                              std::string(snd_strerror(recoverError)).c_str());
            if (IsDeviceDisconnected(recoverError)) {
                CloseDevice();
            }
        }
        return;
    }
    if (state != SND_PCM_STATE_RUNNING && state != SND_PCM_STATE_DRAINING) {
        return;
    }

    auto err = snd_pcm_drain(iHandle);
    if (err < 0) {
        if (IsDeviceDisconnected(err)) {
            OhLog::PrintError("DriverAlsa: DAC disconnected during drain (%s). Closing handle.\n",
                              snd_strerror(err));
            CloseDevice();
            return;
        }
        state = snd_pcm_state(iHandle);
        OhLog::PrintError("DriverAlsa: snd_pcm_drain() error: %s (state=%d)\n",
                          std::string(snd_strerror(err)).c_str(), state);
        const int recoverError = snd_pcm_recover(iHandle, err, 1);
        if (recoverError < 0) {
            if (IsDeviceDisconnected(recoverError)) {
                OhLog::PrintError("DriverAlsa: DAC disconnected during drain recovery (%s). Closing handle.\n",
                                  snd_strerror(recoverError));
                CloseDevice();
                return;
            }
            OhLog::PrintError("DriverAlsa: drain recovery failed: %s (state=%d)\n",
                              std::string(snd_strerror(recoverError)).c_str(),
                              snd_pcm_state(iHandle));
        }
        return;
    }

    err = snd_pcm_prepare(iHandle);
    if (err < 0) {
        if (IsDeviceDisconnected(err)) {
            CloseDevice();
            return;
        }
        OhLog::PrintError("DriverAlsa: snd_pcm_prepare() error: %s\n",
                          std::string(snd_strerror(err)).c_str());
        const int recoverError = snd_pcm_recover(iHandle, err, 1);
        if (recoverError < 0) {
            if (IsDeviceDisconnected(recoverError)) {
                CloseDevice();
                return;
            }
            OhLog::PrintError("DriverAlsa: prepare recovery failed: %s (state=%d)\n",
                              std::string(snd_strerror(recoverError)).c_str(),
                              snd_pcm_state(iHandle));
        }
    }
}

void DriverAlsaPimpl::Write(const Brx& aData)
{
    std::unique_lock<std::mutex> lock(iAlsaMutex);
    if (iHandle == nullptr) {
        if (!ReconfigureCurrentStreamLocked()) {
            return;
        }
    }

    if (iSampleBytes == 0 || (aData.Bytes() % iSampleBytes) != 0) {
        return;
    }

    const TByte* ptr = aData.Ptr();
    snd_pcm_uframes_t framesRemaining = aData.Bytes() / iSampleBytes;
    TUint recoveryAttempts = 0;
    TUint retryTimeouts = 0;
    const TUint kMaxRecoveryAttempts = 5;
    while (framesRemaining > 0) {
        snd_pcm_sframes_t framesWritten = snd_pcm_writei(iHandle, ptr, framesRemaining);

        if (framesWritten < 0) {
            if (IsDeviceDisconnected(static_cast<int>(framesWritten))) {
                OhLog::PrintError("DriverAlsa: DAC disconnected (%s). Closing handle for auto-reconnect.\n",
                                  snd_strerror(static_cast<int>(framesWritten)));
                CloseDevice();
                iDitch = true;
                return;
            }

            const int err = snd_pcm_recover(iHandle, static_cast<int>(framesWritten), 1);
            if (err < 0) {
                OhLog::PrintError("DriverAlsa: snd_pcm_recover failed: %s\n", snd_strerror(err));
                if (IsDeviceDisconnected(err)) {
                    CloseDevice();
                    iDitch = true;
                }
                return;
            }

            if (++recoveryAttempts > kMaxRecoveryAttempts) {
                return;
            }
            ++iRecoverCount;
            continue;
        }

        if (framesWritten == 0) {
            const int err = snd_pcm_wait(iHandle, 500);
            if (err < 0) {
                if (IsDeviceDisconnected(err)) {
                    CloseDevice();
                    iDitch = true;
                }
                return;
            }
            if (err == 0 && ++retryTimeouts > kMaxRecoveryAttempts) {
                return;
            }
            if (err > 0) {
                retryTimeouts = 0;
            }
            continue;
        }

        const auto framesConsumed = static_cast<snd_pcm_uframes_t>(framesWritten);
        const TUint bytesConsumed = framesConsumed * iSampleBytes;
        ptr += bytesConsumed;
        framesRemaining -= framesConsumed;
        iBytesSent += bytesConsumed;
        recoveryAttempts = 0;
        retryTimeouts = 0;
    }
}

#ifdef DEBUG
void DriverAlsaPimpl::LogPCMState()
{
    std::lock_guard<std::mutex> lock(iAlsaMutex);
    if (iHandle == nullptr) {
        OhLog::PrintDebug("DriverAlsa: PCM STATE: NULL\n");
        return;
    }
    switch (snd_pcm_state(iHandle))
    {
        case SND_PCM_STATE_OPEN:
            OhLog::PrintDebug("DriverAlsa: PCM STATE: SND_PCM_STATE_OPEN\n");
            break;
        case SND_PCM_STATE_SETUP:
            OhLog::PrintDebug("DriverAlsa: PCM STATE: SND_PCM_STATE_SETUP\n");
            break;
        case SND_PCM_STATE_PREPARED:
            OhLog::PrintDebug("DriverAlsa: PCM STATE: SND_PCM_STATE_PREPARED\n");
            break;
        case SND_PCM_STATE_RUNNING:
            OhLog::PrintDebug("DriverAlsa: PCM STATE: SND_PCM_STATE_RUNNING\n");
            break;
        case SND_PCM_STATE_XRUN:
            OhLog::PrintDebug("DriverAlsa: PCM STATE: SND_PCM_STATE_XRUN\n");
            break;
        case SND_PCM_STATE_DRAINING:
            OhLog::PrintDebug("DriverAlsa: PCM STATE: SND_PCM_STATE_DRAINING\n");
            break;
        case SND_PCM_STATE_PAUSED:
            OhLog::PrintDebug("DriverAlsa: PCM STATE: SND_PCM_STATE_PAUSED\n");
            break;
        case SND_PCM_STATE_SUSPENDED:
            OhLog::PrintDebug("DriverAlsa: PCM STATE: SND_PCM_STATE_SUSPENDED\n");
            break;
        case SND_PCM_STATE_DISCONNECTED:
            OhLog::PrintDebug("DriverAlsa: PCM STATE: SND_PCM_STATE_DISCONNECTED\n");
            break;
        default:
            OhLog::PrintDebug("DriverAlsa: PCM STATE: UNKNOWN\n");
            break;
    }
}
#endif

void DriverAlsaPimpl::ProcessDecodedStream(MsgDecodedStream* aMsg)
{
 
 
    
    if (iProfile == nullptr) {
	Log::Print("DriverAlsa: iProfile nullptr");
        iDitch = true;
        iSampleBytes = 0;
        iHasStreamSpec = false;
        return;
    }

    if (iSampleBytes != 0) {
        ProcessDrain();
    }

    const auto & decodedStreamInfo = aMsg->StreamInfo();

    Log::Print("DriverAlsa: Bytes Sent since last MsgDecodedStream = %u\n",
               iBytesSent);

    iBytesSent = 0;
    iXrunCount = 0;
    iRecoverCount = 0;
    iWaitCount = 0;


    Log::Print("DriverAlsa: Finding PcmProcessor for stream: BitDepth = %u, "
               "SampleRate = %u, Channels = %u\n",
               decodedStreamInfo.BitDepth(), decodedStreamInfo.SampleRate(),
               decodedStreamInfo.NumChannels());

    // Mono plays badly on the Raspberry Pi and causes issues when
    // switching to a stereo track.
    //
    // So we configure the playback for stereo and duplicate the
    // channel data.
    const TBool duplicateChannel = decodedStreamInfo.NumChannels() == 1;

    const TUint outputRate = iOutputSampleRate ? iOutputSampleRate
                                                : decodedStreamInfo.SampleRate();
    iCurrentOutputRate.store(outputRate);

    const TUint bitDepth = decodedStreamInfo.BitDepth();
    KfrPcmProcessor& pcmProcessor =
        static_cast<KfrPcmProcessor&>(iProfile->GetPcmProcessor());
    AudioSpec spec{};
    spec.iNumChannels = decodedStreamInfo.NumChannels();
    spec.iBitDepth = bitDepth;
    spec.iInputRate = decodedStreamInfo.SampleRate();
    spec.iOutputRate = outputRate;
    spec.iLowLatency = iLowLatencyMode;

    pcmProcessor.SetDuplicateChannel(duplicateChannel);
    pcmProcessor.UpdateFormatSpec(spec);

    std::lock_guard<std::mutex> lock(iAlsaMutex);
    iDuplicateChannel = duplicateChannel;
    iLastBitDepth = bitDepth;
    iLastNumChannels = decodedStreamInfo.NumChannels();
    iLastInputRate = decodedStreamInfo.SampleRate();
    iHasStreamSpec = true;

    if (!TryProfileLocked(*iProfile, bitDepth, decodedStreamInfo.NumChannels(),
                          outputRate, iBufferUs)) {
        Log::Print("DriverAlsa: No usable ALSA profile for stream: BitDepth = %u, "
                   "SampleRate = %u, Channels = %u\n",
                   bitDepth, decodedStreamInfo.SampleRate(),
                   decodedStreamInfo.NumChannels());
        iSampleBytes = 0;
        iDitch = true;
        return;
    }

    const OutputFormat outputFormat = iProfile->GetFormat(bitDepth);
    const TUint outputChannels =
        (iDuplicateChannel && spec.iNumChannels == 1) ? 2 : spec.iNumChannels;
    iSampleBytes = outputChannels * outputFormat.second;
    iDitch = false;
}

TBool DriverAlsaPimpl::TryProfileLocked(Profile& aProfile,
                                        TUint aBitDepth,
                                        TUint aNumChannels,
                                        TUint aSampleRate,
                                        TUint aBufferUs)
{
    if (aBitDepth != 32 && aBitDepth != 24 &&
        aBitDepth != 16 && aBitDepth != 8) {
        return false;
    }


    if (!EnsureDeviceOpen()) {
       OhLog::PrintError("DriverAlsa: Device not open yet in ProcessDecodedStream; will retry in Write()\n");
       iDitch = true;
       return false;
    }


    auto outputFormat = aProfile.GetFormat(aBitDepth);

    snd_pcm_hw_params_t* hwParams;
    snd_pcm_sw_params_t* swParams;
    snd_pcm_hw_params_alloca(&hwParams);
    snd_pcm_sw_params_alloca(&swParams);

    auto fail = [this](const char* aOperation, int aError) -> TBool {
        Log::Print("DriverAlsa: %s failed: %s (%d)\n",
                   aOperation, snd_strerror(aError), aError);
        if (IsDeviceDisconnected(aError)) {
            CloseDevice();
        }
        return false;
    };

    const snd_pcm_state_t state = snd_pcm_state(iHandle);
    if (state != SND_PCM_STATE_OPEN) {
        const int dropError = snd_pcm_drop(iHandle);
        if (dropError < 0 && dropError != -EBADFD) {
            return fail("snd_pcm_drop", dropError);
        }
        const int freeError = snd_pcm_hw_free(iHandle);
        if (freeError < 0) {
            return fail("snd_pcm_hw_free", freeError);
        }
    }

    int err = snd_pcm_hw_params_any(iHandle, hwParams);
    if (err < 0) return fail("snd_pcm_hw_params_any", err);

    err = snd_pcm_hw_params_set_access(iHandle, hwParams,
                                       SND_PCM_ACCESS_RW_INTERLEAVED);
    if (err < 0) return fail("snd_pcm_hw_params_set_access", err);

    err = snd_pcm_hw_params_set_format(iHandle, hwParams,
                                       outputFormat.first);
    if (err < 0) return fail("snd_pcm_hw_params_set_format", err);

    const TUint outputChannels =
        (iDuplicateChannel && aNumChannels == 1) ? 2 : aNumChannels;

    err = snd_pcm_hw_params_set_channels(
        iHandle,
        hwParams,
        outputChannels);
    if (err < 0) {
        return fail("snd_pcm_hw_params_set_channels", err);
    }

    unsigned int rate = aSampleRate;
    err = snd_pcm_hw_params_set_rate_near(iHandle, hwParams, &rate, nullptr);
    if (err < 0) return fail("snd_pcm_hw_params_set_rate_near", err);
    if (rate != aSampleRate) {
        Log::Print("DriverAlsa: ALSA rate mismatch: requested=%u negotiated=%u\n",
                   aSampleRate, rate);
        return false;
    }

    const uint64_t requestedFrames =
        (static_cast<uint64_t>(aBufferUs) * aSampleRate + 999999) / 1000000;
    const snd_pcm_uframes_t requestedBufferFrames =
        static_cast<snd_pcm_uframes_t>(std::max<uint64_t>(1, requestedFrames));
    snd_pcm_uframes_t periodSize = iLowLatencyMode
        ? std::max<snd_pcm_uframes_t>(1, requestedBufferFrames / 4)
        : 1024;
    err = snd_pcm_hw_params_set_period_size_near(iHandle,
                                                 hwParams,
                                                 &periodSize,
                                                 nullptr);
    if (err < 0) return fail("snd_pcm_hw_params_set_period_size_near", err);

    const snd_pcm_uframes_t minimumBufferFrames = iLowLatencyMode
        ? 2 * periodSize
        : 8192;
    snd_pcm_uframes_t bufferSize = std::max(minimumBufferFrames, requestedBufferFrames);
    err = snd_pcm_hw_params_set_buffer_size_near(iHandle,
                                                 hwParams,
                                                 &bufferSize);
    if (err < 0) return fail("snd_pcm_hw_params_set_buffer_size_near", err);
    if (bufferSize < minimumBufferFrames) {
        Log::Print("DriverAlsa: ALSA buffer below required minimum: %lu frames\n",
                   bufferSize);
        return false;
    }

    err = snd_pcm_hw_params(iHandle, hwParams);
    if (err < 0) return fail("snd_pcm_hw_params", err);

    err = snd_pcm_hw_params_get_buffer_size(hwParams, &bufferSize);
    if (err < 0) return fail("snd_pcm_hw_params_get_buffer_size", err);
    if (bufferSize < minimumBufferFrames) {
        Log::Print("DriverAlsa: configured ALSA buffer below required minimum: %lu frames\n",
                   bufferSize);
        return false;
    }
    err = snd_pcm_hw_params_get_period_size(hwParams, &periodSize, nullptr);
    if (err < 0) return fail("snd_pcm_hw_params_get_period_size", err);

    err = snd_pcm_sw_params_current(iHandle, swParams);
    if (err < 0) return fail("snd_pcm_sw_params_current", err);

    err = snd_pcm_sw_params_set_start_threshold(iHandle,
                                                swParams,
                                                bufferSize / 2);
    if (err < 0) return fail("snd_pcm_sw_params_set_start_threshold", err);

    err = snd_pcm_sw_params_set_avail_min(iHandle,
                                          swParams,
                                          periodSize);
    if (err < 0) return fail("snd_pcm_sw_params_set_avail_min", err);

    err = snd_pcm_sw_params(iHandle, swParams);
    if (err < 0) return fail("snd_pcm_sw_params", err);

    snd_pcm_uframes_t startThreshold = 0;
    snd_pcm_uframes_t availMin = 0;
    snd_pcm_sw_params_get_start_threshold(swParams, &startThreshold);
    snd_pcm_sw_params_get_avail_min(swParams, &availMin);

    err = snd_pcm_prepare(iHandle);
    if (err < 0) return fail("snd_pcm_prepare", err);

        Log::Print("DriverAlsa: configured ALSA: rate=%u channels=%u "
            "format=%d buffer=%lu frames period=%lu frames lowLatency=%u\n",
           rate,
           outputChannels,
           outputFormat.first,
           bufferSize,
            periodSize,
            iLowLatencyMode);

    return true;
}

TUint DriverAlsaPimpl::DriverDelayJiffies(TUint aSampleRate)
{
    std::lock_guard<std::mutex> lock(iAlsaMutex);
    const TUint dspDelay = iProfile == nullptr
        ? 0
        : static_cast<KfrPcmProcessor&>(iProfile->GetPcmProcessor()).GetDelayJiffies();
    const TUint rate = iCurrentOutputRate.load() != 0
        ? iCurrentOutputRate.load()
        : aSampleRate;

    if (iHandle == nullptr || !aSampleRate) {
        return dspDelay;
    }

    snd_pcm_sframes_t delayFrames = 0;
    // While the stream is (re)configured - e.g. a live radio switching sample
    // rate 24k->48k - the handle is in SETUP/OPEN state and snd_pcm_delay()
    // returns -EBADF. That is expected, not an error: report no delay.
    const snd_pcm_state_t state = snd_pcm_state(iHandle);
    if (state != SND_PCM_STATE_PREPARED && state != SND_PCM_STATE_RUNNING
        && state != SND_PCM_STATE_PAUSED && state != SND_PCM_STATE_XRUN) {
        return dspDelay;
    }
    int ret = snd_pcm_delay(iHandle, &delayFrames);

    if (ret < 0) {
        // Do not call snd_pcm_recover here: DriverDelayJiffies is called
        // from the pipeline animator thread and can race with AudioThread writes.
        return dspDelay;
    }

    if (delayFrames < 0) {
        delayFrames = 0;
    }

    const uint64_t totalDelay = static_cast<uint64_t>(delayFrames) *
                                Jiffies::PerSample(rate) + dspDelay;
    return static_cast<TUint>(std::min<uint64_t>(
        totalDelay, std::numeric_limits<TUint>::max()));
}

void DriverAlsaPimpl::ProcessMode(const Brx& aMode)
{
    const TBool lowLatencyMode = aMode == Brn("ALSA_CAPTURE");
    if (iProfile != nullptr) {
        iProfile->GetPcmProcessor().Flush();
    }

    std::lock_guard<std::mutex> lock(iAlsaMutex);
    iLowLatencyMode = lowLatencyMode;
    OhLog::Print(
        "DriverAlsa: MsgMode intercepted. lowLatency=%u; purging pipeline tracking state.\n",
        iLowLatencyMode);

    iSampleBytes = 0;
    iDitch = true;
    iBytesSent = 0;
    iHasStreamSpec = false;
}

DriverAlsa::DriverAlsa(IPipeline& aPipeline,
                       const Brx& aAlsaDevice,
                       TUint aBufferUs,
                       TUint aOutputSampleRate,
                       const Brx& aKfrIrConfigDir)
: PipelineDriverBase(aPipeline, std::make_unique<DriverAlsaPimpl>(
      aAlsaDevice, aBufferUs, aOutputSampleRate, aKfrIrConfigDir))
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
{
    iThread.reset();
    iBackend.reset();
}

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
    Brhz modeNameCStr(modeName); 
    OhLog::Print("DriverAlsa: MsgMode intercepted. %s \n", modeNameCStr.CString());

    iBackend->ProcessMode(aMsg->Mode());

    return aMsg;
}

Msg* PipelineDriverBase::ProcessMsg(MsgDrain* aMsg)
{
    // Ensure the ALSA audio buffer is emptied.
    iBackend->ProcessDrain();
    aMsg->ReportDrained();

    return aMsg;
}
