#include "ProtocolAlsaCapture.h"
#include <OpenHome/Private/Printer.h>
#include <OpenHome/Private/Parser.h>
#include <OpenHome/Private/Ascii.h>
#include <OpenHome/Private/Thread.h>
#include <OpenHome/Media/SupplyAggregator.h>
#include <alsa/asoundlib.h>
#include <condition_variable>
#include <deque>
#include <mutex>
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
    {
        for (TUint i = 0; i < kQueueCapacity; ++i) {
            iFreeSlots.push_back(&iPool[i]);
        }
    }

    void Interrupt()
    {
        {
            std::lock_guard<std::mutex> lock(iMutex);
            iInterrupted = true;
        }
        iReadyCondition.notify_all();
    }

    void ResetInterrupt()
    {
        std::lock_guard<std::mutex> lock(iMutex);
        iInterrupted = false;
    }

    AudioChunk* GetFreeChunk(bool& aDroppedOldest)
    {
        std::lock_guard<std::mutex> lock(iMutex);
        aDroppedOldest = false;
        if (!iFreeSlots.empty()) {
            AudioChunk* chunk = iFreeSlots.front();
            iFreeSlots.pop_front();
            return chunk;
        }
        if (!iReadySlots.empty()) {
            AudioChunk* chunk = iReadySlots.front();
            iReadySlots.pop_front();
            chunk->SetBytes(0);
            aDroppedOldest = true;
            return chunk;
        }
        else {
            return nullptr;
        }
    }

    bool PushReadyChunk(AudioChunk* aChunk)
    {
        if (aChunk == nullptr) {
            return false;
        }

        {
            std::lock_guard<std::mutex> lock(iMutex);
            if (iInterrupted) {
                return false;
            }
            iReadySlots.push_back(aChunk);
        }
        iReadyCondition.notify_one();
        return true;
    }

    AudioChunk* PullReadyChunk()
    {
        std::unique_lock<std::mutex> lock(iMutex);
        iReadyCondition.wait(lock, [this] {
            return iInterrupted || !iReadySlots.empty();
        });
        if (iInterrupted || iReadySlots.empty()) {
            return nullptr;
        }
        AudioChunk* chunk = iReadySlots.front();
        iReadySlots.pop_front();
        return chunk;
    }

    void RecycleChunk(AudioChunk* aChunk)
    {
        if (aChunk == nullptr) {
            return;
        }

        aChunk->SetBytes(0);
        std::lock_guard<std::mutex> lock(iMutex);
        iFreeSlots.push_back(aChunk);
    }

    void DrainReadyChunks()
    {
        std::lock_guard<std::mutex> lock(iMutex);
        while (!iReadySlots.empty()) {
            AudioChunk* chunk = iReadySlots.front();
            iReadySlots.pop_front();
            chunk->SetBytes(0);
            iFreeSlots.push_back(chunk);
        }
    }

    TUint ReadySlotsUsed() const
    {
        std::lock_guard<std::mutex> lock(iMutex);
        return static_cast<TUint>(iReadySlots.size());
    }

private:
    static constexpr TUint kQueueCapacity = 8;

    AudioChunk iPool[kQueueCapacity];
    std::deque<AudioChunk*> iFreeSlots;
    std::deque<AudioChunk*> iReadySlots;
    mutable std::mutex iMutex;
    std::condition_variable iReadyCondition;
    bool iInterrupted{false};
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

    void DrainReadyChunks();

    void OutputDrain();

    void StartCaptureThread();

    void StopCaptureThread();

    void CaptureThreadLoop();

    ProtocolStreamResult RunStreamPipeline(IStreamHandler& aStreamHandler, TUint aStreamId, const Brx& aUri);

private:
    const Environment*         iEnv;
    snd_pcm_t*                 iCaptureHandle;
    Mutex                      iCaptureLock;
    std::atomic<bool>          iInterrupted;
    Media::SupplyAggregator*   iSupply;
    Uri                        iUri;
    Semaphore                  iSemDrain;
    Semaphore                  iSemPrefill;
    Bws<64> iDeviceName;
    TUint iSampleRate;
    TUint iChannels;
    OpenHomeAudioPool          iAudioPool; // Native Object Queue replaces the custom ring buffer
    ThreadFunctor*             iCaptureThread;

    static constexpr TUint     kDefaultSampleRate  = 44100;
    static constexpr TUint     kDefaultChannels    = 2;
    static constexpr TUint     kBitDepth           = 32;
    static constexpr TUint     kSampleSizeBytes    = 4;
    static constexpr TUint     kDefaultDelayJiffies = Jiffies::kPerMs * 100;
    static constexpr TUint     kInitialPrefillChunks = 5;
};

ProtocolAlsaCapture::Pimpl::Pimpl(const DevParam& aDevParam, Environment& aEnv)
: iEnv(&aEnv)
, iCaptureHandle(nullptr)
, iCaptureLock("pac")
, iInterrupted(false)
, iSupply(nullptr)
, iSemDrain("PACD", 0)
, iSemPrefill("PACF", 0)
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
    iSupply = new SupplyAggregatorJiffies(aMsgFactory, aDownstream);
}

void ProtocolAlsaCapture::Pimpl::Interrupt(TBool aInterrupt)
{
    iInterrupted.store(aInterrupt);

    if (aInterrupt) {
        iAudioPool.Interrupt();
        iSemPrefill.Signal();

        AutoMutex _(iCaptureLock);
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
    int err = 0;
    {
        AutoMutex _(iCaptureLock);
        err = snd_pcm_open(
            &iCaptureHandle,
            targetDevice,
            SND_PCM_STREAM_CAPTURE,
            SND_PCM_NONBLOCK);

        if (err >= 0) {
            err = snd_pcm_set_params(iCaptureHandle,
                    SND_PCM_FORMAT_S32_LE,
                    SND_PCM_ACCESS_RW_INTERLEAVED,
                    iChannels,
                    iSampleRate, 1, 100000);
        }

        if (err >= 0) {
            snd_pcm_hw_params_t* hwParams = nullptr;
            snd_pcm_hw_params_alloca(&hwParams);
            err = snd_pcm_hw_params_current(iCaptureHandle, hwParams);
            if (err >= 0) {
                unsigned int actualRate = 0;
                int dir = 0;
                err = snd_pcm_hw_params_get_rate(hwParams, &actualRate, &dir);
                if (err >= 0) {
                    if (iSampleRate != actualRate) {
                        Log::Print("ProtocolAlsaCapture: Clock adjusted. Requested: %uHz, Running: %uHz\n",
                                   iSampleRate, actualRate);
                        iSampleRate = actualRate;
                    }
                    err = snd_pcm_start(iCaptureHandle);
                }
            }
        }
    }

    if (err < 0) {
        Log::Print("ProtocolAlsaCapture: ALSA capture setup failed: %s\n",
                   snd_strerror(err));
        CloseHardwareCapture();
        return false;
    }
    return true;
}

void ProtocolAlsaCapture::Pimpl::CloseHardwareCapture()
{
    AutoMutex _(iCaptureLock);
    if (iCaptureHandle != nullptr) {
        snd_pcm_close(iCaptureHandle);
        iCaptureHandle = nullptr;
    }
}

void ProtocolAlsaCapture::Pimpl::DrainReadyChunks()
{
    iAudioPool.DrainReadyChunks();
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
    iSemPrefill.Signal();

    {
        AutoMutex _(iCaptureLock);
        if (iCaptureHandle != nullptr) {
            snd_pcm_drop(iCaptureHandle);
        }
    }

    delete iCaptureThread;
    iCaptureThread = nullptr;
}

void ProtocolAlsaCapture::Pimpl::CaptureThreadLoop()
{
    constexpr TUint kFramesPerRead = 1024;
    const TUint bytesPerFrame = iChannels * kSampleSizeBytes;
    std::vector<TByte> localBuffer(kFramesPerRead * bytesPerFrame);
    TUint framesCollected = 0;
    TUint droppedBlocks = 0;

    while (!iInterrupted.load()) {
        snd_pcm_sframes_t framesRead = 0;
        int recoverStatus = 0;
        {
            AutoMutex _(iCaptureLock);
            if (iCaptureHandle == nullptr) {
                framesRead = -ENODEV;
                recoverStatus = -ENODEV;
            }
            else {
                auto* readBuffer = localBuffer.data() + framesCollected * bytesPerFrame;
                const TUint framesRequested = kFramesPerRead - framesCollected;
                framesRead = snd_pcm_readi(iCaptureHandle, readBuffer, framesRequested);
                if (framesRead == -EAGAIN) {
                    const int waitStatus = snd_pcm_wait(iCaptureHandle, 100);
                    if (waitStatus < 0) {
                        framesRead = waitStatus;
                        recoverStatus = snd_pcm_recover(iCaptureHandle, waitStatus, 1);
                    }
                }
                else if (framesRead < 0) {
                    recoverStatus = snd_pcm_recover(iCaptureHandle, framesRead, 1);
                }
            }
        }

        if (framesRead == -EAGAIN) {
            continue;
        }

        if (framesRead < 0) {
            if (recoverStatus < 0) {
                Log::Print("ProtocolAlsaCapture: ERROR - S/PDIF clock lost or device disconnected (%s). Exiting worker.\n", 
                           std::string(snd_strerror(recoverStatus)).c_str());
                iInterrupted.store(true);
                iAudioPool.Interrupt();
                iSemPrefill.Signal();
                break;
            }
            framesCollected = 0;
            continue;
        }

        if (framesRead > 0) {
            framesCollected += static_cast<TUint>(framesRead);
            if (framesCollected < kFramesPerRead) {
                continue;
            }
            framesCollected = 0;

            const TUint bytesRead = kFramesPerRead * bytesPerFrame;
            bool droppedOldest = false;
            OpenHomeAudioPool::AudioChunk* chunk = iAudioPool.GetFreeChunk(droppedOldest);
            if (chunk == nullptr) {
                ++droppedBlocks;
                if (droppedBlocks == 1 || droppedBlocks % 100 == 0) {
                    Log::Print("ProtocolAlsaCapture: Dropped %u capture blocks; all chunks are in flight\n",
                               droppedBlocks);
                }
                continue;
            }
            if (droppedOldest) {
                ++droppedBlocks;
                if (droppedBlocks == 1 || droppedBlocks % 100 == 0) {
                    Log::Print("ProtocolAlsaCapture: Dropped %u stale capture blocks\n",
                               droppedBlocks);
                }
            }

            if (iInterrupted.load()) {
                iAudioPool.RecycleChunk(chunk);
                break;
            }

            if (bytesRead > AudioData::kMaxBytes) {
                iAudioPool.RecycleChunk(chunk);
                Log::Print("ProtocolAlsaCapture: Capture block exceeds PCM message capacity\n");
                iInterrupted.store(true);
                iAudioPool.Interrupt();
                iSemPrefill.Signal();
                break;
            }

            chunk->Replace(localBuffer.data(), bytesRead);
            if (!iAudioPool.PushReadyChunk(chunk)) {
                iAudioPool.RecycleChunk(chunk);
                break;
            }
            iSemPrefill.Signal();
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
    //OutputDrain();

    iSemPrefill.Clear();
    StartCaptureThread();
    while (!iInterrupted.load() &&
           iAudioPool.ReadySlotsUsed() < kInitialPrefillChunks) {
        iSemPrefill.Wait();
    }
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
    if (!aUri.BeginsWith(Brn("alsacapture://"))) {
        return EProtocolErrorNotSupported;
    }
    iImpl->Interrupt(false);
    iImpl->DrainReadyChunks();
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