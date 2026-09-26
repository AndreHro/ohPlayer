#include "DriverAlsa.h"
#include "ConvertBELE.h"
#include "OhLog.h"
#include <OpenHome/Private/Printer.h>
#include <OpenHome/Net/Private/Globals.h>

#include <memory>
#include <cerrno>
#include <cstdio>
#include <cstring>
#include <vector>
#include <cstdint>
#include <unistd.h>
#include <fcntl.h>
#include <sys/wait.h>

using namespace OpenHome;
using namespace OpenHome::Media;

class CamillaDspDriverPimpl final : public IDriverBackend
{
public:
    CamillaDspDriverPimpl(const Brx& aConfigDir, TUint aBufferUs);
    ~CamillaDspDriverPimpl();
    void ProcessDecodedStream(MsgDecodedStream* aMsg);
    void ProcessPlayable(MsgPlayable* aMsg);
    void ProcessDrain();
    void ProcessMode();
    TUint DriverDelayJiffies(TUint aSampleRate);
private:
    void WritePipe(const TByte* aData, TUint aBytes);
    void LaunchProcess(TUint aSampleRate);
    void StopProcess();
    void ConvertAndWrite(const Brx& aData, TUint aNumChannels, TUint aSubsampleBytes);

    class PipePcmProcessor : public IPcmProcessor
    {
    public:
        PipePcmProcessor(CamillaDspDriverPimpl& aDriver) : iDriver(aDriver) {}
        void BeginBlock() override {}
        void ProcessFragment(const Brx& aData, TUint aNumChannels, TUint aSubsampleBytes) override
        {
            iDriver.ConvertAndWrite(aData, aNumChannels, aSubsampleBytes);
        }
        void ProcessSilence(const Brx& aData, TUint aNumChannels, TUint aSubsampleBytes) override
        {
            iDriver.ConvertAndWrite(aData, aNumChannels, aSubsampleBytes);
        }
        void EndBlock() override {}
        void Flush() override {}
    private:
        CamillaDspDriverPimpl& iDriver;
    };

private:
    FILE* iFileHandle;
    Bws<256> iConfigDir;
    TBool iDuplicateChannel;
    TBool iDitch;
    TUint iBytesSent;
    TUint iOutputSampleRate;
    TUint iCurrentOutputRate;

    TBool iConfigured;
    PipePcmProcessor iProcessor;

    std::vector<TByte> iConversionBuffer;
    std::vector<TInt32> iTmpOut;
    std::vector<TByte> iReusableInputBuffer;

    std::vector<TByte> iRemainderBuffer;
    TUint iRemainderChannels;
    TUint iRemainderSampleBytes;

    TBool iUseStdout;
};

// Note: SIGPIPE handling is process-wide. Configure once during
// application startup (signal(SIGPIPE, SIG_IGN)) rather than
// in this driver constructor.

CamillaDspDriverPimpl::CamillaDspDriverPimpl(const Brx& aConfigDir, TUint aBufferUs)
: iFileHandle(nullptr)
, iConfigDir(aConfigDir)
, iDuplicateChannel(false)
, iDitch(false)
, iBytesSent(0)
, iOutputSampleRate(0)
, iCurrentOutputRate(0)
, iConfigured(false)
, iProcessor(*this)
, iConversionBuffer()
, iRemainderBuffer()
, iRemainderChannels(0)
, iRemainderSampleBytes(0)
, iUseStdout(false)
{
    (void) aBufferUs;
}

CamillaDspDriverPimpl::~CamillaDspDriverPimpl()
{
    StopProcess();
}

void CamillaDspDriverPimpl::LaunchProcess(TUint aSampleRate)
{
    StopProcess();
    iUseStdout = false;
    iCurrentOutputRate = 0;

    if (iConfigDir.Bytes() == 0) {
        iFileHandle = stdout;
        iUseStdout = true;
        OhLog::Print("CamillaDspDriver: Routing audio directly to stdout.\n");
    } else {
        Bws<512> cmd;
        // Quote config dir path to prevent shell injection from paths
        // containing spaces, semicolons, backticks, etc.
        cmd.AppendPrintf("camilladsp -c \"%s/config_%dhz.yaml\"",
                         PBUF(iConfigDir), aSampleRate);

        OhLog::Print("CamillaDspDriver: Spawning: %s\n", PBUF(cmd));

        iFileHandle = popen(cmd.PtrZ(), "w");
        if (!iFileHandle) {
            OhLog::PrintError("CamillaDspDriver: popen failed: %s\n", std::strerror(errno));
            iDitch = true;
            return;
        }
        iUseStdout = false;
    }

    int pipeFd = fileno(iFileHandle);
    (void)fcntl(pipeFd, F_SETPIPE_SZ, 1048576);
    std::setvbuf(iFileHandle, nullptr, _IONBF, 0);
}

void CamillaDspDriverPimpl::StopProcess()
{
    if (iFileHandle) {
        std::fflush(iFileHandle);
        if (!iUseStdout) {
            // Use alarm to prevent pclose from blocking the audio thread
            // if the child process hangs. alarm() is process-wide but
            // acceptable for an audio application where a 2-second
            // timeout on mode changes is tolerable.
            alarm(2);
            int status;
            pid_t pid = pclose(iFileHandle);
            alarm(0);
            // If alarm interrupted popen, child may still be running.
            // Non-blocking wait to reap any already-exited child.
            (void)pid;
            (void)status;
        }
        iFileHandle = nullptr;
    }
    // Reap any zombie children left by interrupted pclose.
    while (waitpid(-1, nullptr, WNOHANG) > 0) {}
}

void CamillaDspDriverPimpl::WritePipe(const TByte* aData, TUint aBytes)
{
    if (!iFileHandle) {
        return;
    }

    const TByte* ptr = aData;
    TUint bytesRemaining = aBytes;

    while (bytesRemaining > 0) {
        size_t written = std::fwrite(ptr, 1, bytesRemaining, iFileHandle);
        if (written == 0) {
            if (std::ferror(iFileHandle)) {
                OhLog::PrintError("CamillaDspDriver: fwrite error: %s\n",
                                   std::strerror(errno));
                iDitch = true;
                return;
            }
            break;
        }
        ptr += written;
        bytesRemaining -= written;
        iBytesSent += written;
    }
}

void CamillaDspDriverPimpl::ConvertAndWrite(const Brx& aData, TUint aNumChannels, TUint aSubsampleBytes)
{
    if (aData.Bytes() == 0 ||
        aNumChannels == 0 ||
        aSubsampleBytes < 1 ||
        aSubsampleBytes > 4) {
        return;
    }

    // A remainder can only be combined with the same PCM format.
    if (!iRemainderBuffer.empty() &&
        (iRemainderChannels != aNumChannels ||
         iRemainderSampleBytes != aSubsampleBytes)) {
        OhLog::PrintWarning(
            "CamillaDspDriver: PCM format changed with pending remainder; "
            "dropping remainder\n");

        iRemainderBuffer.clear();
        iRemainderChannels = 0;
        iRemainderSampleBytes = 0;
    }

    std::vector<TByte>& inputBuffer = iReusableInputBuffer;
    inputBuffer.clear();

    if (!iRemainderBuffer.empty()) {
        inputBuffer.insert(inputBuffer.end(),
                           iRemainderBuffer.begin(),
                           iRemainderBuffer.end());
        iRemainderBuffer.clear();
        iRemainderChannels = 0;
        iRemainderSampleBytes = 0;
    }
    inputBuffer.insert(inputBuffer.end(),
                       aData.Ptr(),
                       aData.Ptr() + aData.Bytes());

    const TUint inputFrameSize = aNumChannels * aSubsampleBytes;
    const TUint completeBytes =
       (inputBuffer.size() / inputFrameSize) * inputFrameSize;

    const TUint inputFrames = completeBytes / inputFrameSize;
    const TUint remainingBytes =
        static_cast<TUint>(inputBuffer.size() - completeBytes);

    if (remainingBytes != 0) {
        iRemainderBuffer.assign(inputBuffer.data() + completeBytes,
                                inputBuffer.data() + inputBuffer.size());
        iRemainderChannels = aNumChannels;
        iRemainderSampleBytes = aSubsampleBytes;
    }

    if (inputFrames == 0) {
        return;
    }

    const TUint outputChannels =
        (iDuplicateChannel && aNumChannels == 1) ? 2 : aNumChannels;
    const TUint outputBytes =
        inputFrames * outputChannels * sizeof(TInt32);

    if (iConversionBuffer.size() < outputBytes) {
        iConversionBuffer.resize(outputBytes);
    }

    OpenHome::Media::ConvertInterleavedToS32LE_With24Simd(
        inputBuffer.data(),
        completeBytes,
        iConversionBuffer.data(),
        inputFrames,
        aNumChannels,
        aSubsampleBytes,
        false /* OH PIPELINE IS ALWAYS BE */,
        iDuplicateChannel && aNumChannels == 1,
        iTmpOut);

    WritePipe(iConversionBuffer.data(), outputBytes);
}

void CamillaDspDriverPimpl::ProcessPlayable(MsgPlayable* aMsg)
{
    if (!iConfigured || iDitch || !iFileHandle) {
        return;
    }

    aMsg->Read(iProcessor);
}

void CamillaDspDriverPimpl::ProcessDrain()
{
    if (iFileHandle) {
        std::fflush(iFileHandle);
    }
}

void CamillaDspDriverPimpl::ProcessMode()
{
    Log::Print("CamillaDspDriver: MsgMode intercepted. Purging pipeline state.\n");

    if (iFileHandle) {
        std::fflush(iFileHandle);
    }

    iBytesSent = 0;
    iCurrentOutputRate = 0;

    StopProcess();

    iRemainderBuffer.clear();
    iRemainderChannels = 0;
    iRemainderSampleBytes = 0;

    iDitch = false;
    iConfigured = false;
}

TUint CamillaDspDriverPimpl::DriverDelayJiffies(TUint aSampleRate)
{
    const TUint rate = (iCurrentOutputRate != 0) ? iCurrentOutputRate : aSampleRate;

    if (!rate) {
        return 0;
    }

    return 0;
}

void CamillaDspDriverPimpl::ProcessDecodedStream(MsgDecodedStream* aMsg)
{
    const auto& decodedStreamInfo = aMsg->StreamInfo();
    TUint incomingRate = decodedStreamInfo.SampleRate();

    Log::Print("CamillaDspDriver: Bytes Sent since last MsgDecodedStream = %u\n", iBytesSent);
    iBytesSent = 0;

    Log::Print("CamillaDspDriver: Stream: BitDepth = %u, SampleRate = %u, Channels = %u\n",
               decodedStreamInfo.BitDepth(), incomingRate, decodedStreamInfo.NumChannels());

    if (decodedStreamInfo.NumChannels() == 1) {
        iDuplicateChannel = true;
    } else {
        iDuplicateChannel = false;
    }

    switch (decodedStreamInfo.BitDepth()) {
        case 8:
        case 16:
        case 24:
        case 32:
            iDitch = false;
            break;
        default:
            OhLog::PrintWarning("CamillaDspDriver: Unsupported bit depth %u\n", decodedStreamInfo.BitDepth());
            iDitch = true;
            return;
    }

    const TUint targetRate = iOutputSampleRate ? iOutputSampleRate : incomingRate;

    if (targetRate != iCurrentOutputRate ||
        iFileHandle == nullptr ||
        iDitch) {
        LaunchProcess(targetRate);

        if (iFileHandle == nullptr) {
            iCurrentOutputRate = 0;
            iDitch = true;
            return;
        }

        iCurrentOutputRate = targetRate;
        iConfigured = true;
        iDitch = false;
    }
}

CamillaDspDriver::CamillaDspDriver(IPipeline& aPipeline, const Brx& aCamilloDsp, TUint aBufferUs)
: PipelineDriverBase(aPipeline, std::make_unique<CamillaDspDriverPimpl>(aCamilloDsp, aBufferUs) )
{
}