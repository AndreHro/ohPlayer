#include "ProtocolAlsaCapture.h"
#include <OpenHome/Private/Printer.h>
#include <OpenHome/Private/Parser.h>
#include <OpenHome/Private/Ascii.h>
#include <OpenHome/Private/Thread.h>
#include <OpenHome/Private/Fifo.h>
#include <OpenHome/Media/SupplyAggregator.h>
#include <alsa/asoundlib.h>
#include <vector>
#include <atomic>
#include <algorithm>
#include <cerrno>
#include <unistd.h>

using namespace OpenHome;
using namespace OpenHome::Media;

class OpenHomeAudioPool : public INonCopyable
{
public:
    using AudioChunk = Bws<AudioData::kMaxBytes>;

    OpenHomeAudioPool()
        : iFreeSlots(kQueueCapacity)
        , iReadySlots(kQueueCapacity)
    {
        for (TUint i = 0; i < kQueueCapacity; ++i) {
            iFreeSlots.Write(&iPool[i]);
        }
    }

    void Interrupt()
    {
        iFreeSlots.ReadInterrupt(true);
        iReadySlots.ReadInterrupt(true);
    }

    void ResetInterrupt()
    {
        iFreeSlots.ReadInterrupt(false);
        iReadySlots.ReadInterrupt(false);
    }

    AudioChunk* GetFreeChunk()
    {
        try {
            return iFreeSlots.Read();
        }
        catch (FifoReadError&) {
            return nullptr;
        }
    }

    bool PushReadyChunk(AudioChunk* aChunk)
    {
        if (aChunk == nullptr) {
            return false;
        }

        try {
            iReadySlots.Write(aChunk);
            return true;
        }
        catch (FifoReadError&) {
            return false;
        }
    }

    AudioChunk* PullReadyChunk()
    {
        try {
            return iReadySlots.Read();
        }
        catch (FifoReadError&) {
            return nullptr;
        }
    }

    void RecycleChunk(AudioChunk* aChunk)
    {
        if (aChunk == nullptr) {
            return;
        }

        aChunk->SetBytes(0);
        iFreeSlots.Write(aChunk);
    }

private:
    static constexpr TUint kQueueCapacity = 64;

    AudioChunk iPool[kQueueCapacity];
    Fifo<AudioChunk*> iFreeSlots;
    Fifo<AudioChunk*> iReadySlots;
};

class ProtocolAlsaCapture::Pimpl
{
public:
    Pimpl(const DevParam& devParam, Environment& aEnv);

    ~Pimpl();

    void Initialise(MsgFactory& aMsgFactory, IPipelineElementDownstream& aDownstream);

    void Interrupt(TBool aInterrupt);

    //void ParseUriParameters(const Brx& aUri);

    bool OpenHardwareCapture();

    void CloseHardwareCapture();

    void OutputDrain();

    void StartCaptureThread();

    void StopCaptureThread();

    void CaptureThreadLoop();

    ProtocolStreamResult RunStreamPipeline(IStreamHandler& aStreamHandler, TUint aStreamId, const Brx& aUri);

private:
    const Environment*         iEnv;
    snd_pcm_t*                 iCaptureHandle;
    std::atomic<bool>          iInterrupted;
    Media::SupplyAggregator*   iSupply;
    Uri                        iUri;
    Semaphore                  iSemDrain;
    Bws<64> iDeviceName;
    TUint iSampleRate;
    TUint iChannels;
    OpenHomeAudioPool          iAudioPool; // Native Object Queue replaces the custom ring buffer
    ThreadFunctor*             iCaptureThread;

    static constexpr TUint     kDefaultSampleRate  = 44100;
    static constexpr TUint     kDefaultChannels    = 2;
    static constexpr TUint     kBitDepth           = 32;
    static constexpr TUint     kSampleSizeBytes    = 4;
    static constexpr TUint     kDefaultDelayJiffies = 0;
};

ProtocolAlsaCapture::Pimpl::Pimpl(const DevParam& aDevParam, Environment& aEnv)
: iEnv(&aEnv)
, iCaptureHandle(nullptr)
, iInterrupted(false)
, iSupply(nullptr)
, iSemDrain("PACD", 0)
, iDeviceName(aDevParam.iDevName)
, iSampleRate(aDevParam.iSampleRate)
, iChannels(aDevParam.iChannels)
, iAudioPool()
, iCaptureThread(nullptr)
{}

ProtocolAlsaCapture::Pimpl::~Pimpl()
{
    StopCaptureThread();
    CloseHardwareCapture();
    delete iSupply;
}

void ProtocolAlsaCapture::Pimpl::Initialise(MsgFactory& aMsgFactory, IPipelineElementDownstream& aDownstream)
{
    iSupply = new SupplyAggregatorBytes(aMsgFactory, aDownstream);
}

void ProtocolAlsaCapture::Pimpl::Interrupt(TBool aInterrupt)
{
    iInterrupted.store(aInterrupt);

    if (aInterrupt) {
        iAudioPool.Interrupt();

        if (iCaptureHandle != nullptr) {
            snd_pcm_drop(iCaptureHandle);
        }
    }
    else {
        iAudioPool.ResetInterrupt();
    }
}

bool ProtocolAlsaCapture::Pimpl::OpenHardwareCapture()
{
    const char* targetDevice = reinterpret_cast<const char*>(iDeviceName.PtrZ());

    int err = snd_pcm_open(
        &iCaptureHandle,
        targetDevice,
        SND_PCM_STREAM_CAPTURE,
        SND_PCM_NONBLOCK);

    if (err < 0) {
        iCaptureHandle = nullptr;
        return false;
    }

    if (snd_pcm_set_params(iCaptureHandle,
            SND_PCM_FORMAT_S32_LE,
            SND_PCM_ACCESS_RW_INTERLEAVED,
            iChannels, 
            iSampleRate, 1, 100000) < 0) {
        CloseHardwareCapture();
        return false;
    }

    snd_pcm_hw_params_t* hwParams = nullptr;
    snd_pcm_hw_params_alloca(&hwParams);

    if (snd_pcm_hw_params_current(iCaptureHandle, hwParams) < 0) {
        CloseHardwareCapture();
        return false;
    }

    unsigned int actualRate = 0;
    int dir = 0;
    if (snd_pcm_hw_params_get_rate(hwParams, &actualRate, &dir) < 0) {
        CloseHardwareCapture();
        return false;
    }

    if (iSampleRate != actualRate) {
        Log::Print("ProtocolAlsaCapture: Clock adjusted. Requested: %uHz, Running: %uHz\n", iSampleRate, actualRate);
        iSampleRate = actualRate;
    }

    return true;
}

void ProtocolAlsaCapture::Pimpl::CloseHardwareCapture()
{
    if (iCaptureHandle != nullptr) {
        snd_pcm_drop(iCaptureHandle);
        iCaptureHandle = nullptr;
    }
}

void ProtocolAlsaCapture::Pimpl::OutputDrain()
{
    if (iSupply == nullptr) return;
    iSemDrain.Clear();
    iSupply->OutputDrain(MakeFunctor(iSemDrain, &Semaphore::Signal));
    try { iSemDrain.Wait(ISupply::kMaxDrainMs); }
    catch (Timeout&) { Log::Print("WARNING: ProtocolAlsaCapture: timeout draining slots\n"); }
}

void ProtocolAlsaCapture::Pimpl::StartCaptureThread()
{
    iCaptureThread = new ThreadFunctor(
        "AlsaCapWorker",
        MakeFunctor(*this, &Pimpl::CaptureThreadLoop),
        kPrioritySystemHighest);
    iCaptureThread->Start();
}

void ProtocolAlsaCapture::Pimpl::StopCaptureThread()
{
    iInterrupted.store(true);
    iAudioPool.Interrupt();

    if (iCaptureHandle != nullptr) {
        snd_pcm_drop(iCaptureHandle);
    }

    delete iCaptureThread;
    iCaptureThread = nullptr;
}

void ProtocolAlsaCapture::Pimpl::CaptureThreadLoop()
{
    const TUint framesPerRead = 256; 
    const TUint bytesPerFrame = iChannels * kSampleSizeBytes;
    std::vector<TByte> localBuffer(framesPerRead * bytesPerFrame);

    while (!iInterrupted.load())
    {
        snd_pcm_sframes_t framesRead = snd_pcm_readi(iCaptureHandle, localBuffer.data(), framesPerRead);
        
        if (framesRead == -EAGAIN) {
            usleep(1000); // Guarding non-blocking poll intervals cleanly
            continue;
        }
        
        if (framesRead < 0) {
            int recoverStatus = snd_pcm_recover(iCaptureHandle, framesRead, 1);
            if (recoverStatus < 0) {
                Log::Print("ProtocolAlsaCapture: ERROR - S/PDIF clock lost or device disconnected (%s). Exiting worker.\n", 
                           snd_strerror(recoverStatus));
                iInterrupted.store(true); // Signal the consumer thread to break out cleanly
                break; // Break the infinite CPU loop entirely to prevent 100% thread spikes
            }
            continue;
        }

        if (framesRead > 0) {
            TUint bytesRead = framesRead * bytesPerFrame;
            TUint srcOffset = 0;
            
            while (bytesRead > 0 && !iInterrupted.load()) {
                OpenHomeAudioPool::AudioChunk* chunk = nullptr;
                try {
                    chunk = iAudioPool.GetFreeChunk(); //
                }
                catch (FifoReadError&) {
                    break;
                }

                if (chunk == nullptr || iInterrupted.load()) { //
                    break;
                }

                const TUint maxChunkBytes = AudioData::kMaxBytes; //
                TUint chunkBytes = std::min<TUint>(bytesRead, maxChunkBytes); //

                chunkBytes -= chunkBytes % bytesPerFrame;

                if (chunkBytes == 0) {
                    // Recycle chunk instantly if packet size falls beneath single frame capacities
                    iAudioPool.RecycleChunk(chunk); //
                    break;
                }
                
                chunk->Replace(&localBuffer[srcOffset], chunkBytes); //
                if (!iAudioPool.PushReadyChunk(chunk)) { //
                    iAudioPool.RecycleChunk(chunk); //
                    break;
                }

                bytesRead -= chunkBytes;
                srcOffset += chunkBytes;
            }
        }
    }
}

ProtocolStreamResult ProtocolAlsaCapture::Pimpl::RunStreamPipeline(IStreamHandler& aStreamHandler, TUint aStreamId, const Brx& aUri)
{
    SpeakerProfile sp;
    PcmStreamInfo streamInfo;
    streamInfo.Set(kBitDepth, iSampleRate, iChannels,
        AudioDataEndian::Little, sp, 0LL);

    iSupply->OutputPcmStream(aUri, 0LL, false, true, Multiroom::Forbidden, aStreamHandler, aStreamId, streamInfo);
    iSupply->OutputDelay(kDefaultDelayJiffies);
    OutputDrain();

    StartCaptureThread();
    ProtocolStreamResult result = EProtocolStreamStopped;

    try {
        while (!iInterrupted.load()) {
            OpenHomeAudioPool::AudioChunk* chunk =
                iAudioPool.PullReadyChunk();

            if (chunk == nullptr) {
                break;
            }

            iSupply->OutputData(*chunk);
            iAudioPool.RecycleChunk(chunk);
        }

        StopCaptureThread();
        iSupply->Flush();
    }
    catch (...) {
        StopCaptureThread();
        iSupply->Flush();
        throw;
    }

    StopCaptureThread();
    return result;
}

//////////////////////////////////////////////////////////////////

ProtocolAlsaCapture::ProtocolAlsaCapture(const DevParam& aDevData, Environment& aEnv)
: Protocol(aEnv)
, iImpl(std::make_unique<ProtocolAlsaCapture::Pimpl>(aDevData, aEnv))
{}

ProtocolAlsaCapture::~ProtocolAlsaCapture() = default;

void ProtocolAlsaCapture::Initialise(MsgFactory& aMsgFactory, IPipelineElementDownstream& aDownstream)
{
    iImpl->Initialise(aMsgFactory, aDownstream);
}

void ProtocolAlsaCapture::Interrupt(TBool aInterrupt)
{
    iImpl->Interrupt(aInterrupt);
}

TUint ProtocolAlsaCapture::TryStop(TUint)
{
    iImpl->Interrupt(true);
    return MsgFlush::kIdInvalid;
}

ProtocolStreamResult ProtocolAlsaCapture::Stream(const Brx& aUri)
{
    iImpl->Interrupt(false);
    //iImpl->ParseUriParameters(aUri);
    if (!iImpl->OpenHardwareCapture()) {
        return EProtocolStreamErrorUnrecoverable;
    }
    TUint streamId = iIdProvider->NextStreamId();
    ProtocolStreamResult res = iImpl->RunStreamPipeline(
        *this, streamId, aUri);
    iImpl->CloseHardwareCapture();
    return res;
}

ProtocolGetResult ProtocolAlsaCapture::Get(IWriter&, const Brx&, TUint64, TUint)
{
    return EProtocolGetErrorNotSupported;
}