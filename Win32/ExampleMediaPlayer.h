#pragma once

#include <OpenHome/Av/MediaPlayer.h>
#include <OpenHome/Av/Utils/DriverSongcastSender.h>
#include <OpenHome/Media/PipelineManager.h>
#include <OpenHome/Media/Tests/VolumeUtils.h>

#include <Windows.h>

namespace OpenHome {
namespace Net {
    class DviServerUpnp;
    class DvStack;
    class DvDevice;
}
namespace Media {
    class PipelineManager;
    class DriverSongcastSender;
}
namespace Configuration {
    class ConfigRegStore;
    class ConfigManager;
}
namespace Av {
    class RamStore;
    class ControlPointProxy;

class ExampleMediaPlayer : private Net::IResourceManager,
                           private Media::IPipelineObserver
{
    static const Brn kSongcastSenderIconFileName;
public:
    ExampleMediaPlayer(LPVOID lpParam, Net::DvStack& aDvStack, const Brx& aUdn,
                       const TChar* aRoom, const TChar* aProductName,
                       const Brx& aUserAgent);
    virtual ~ExampleMediaPlayer();
    Environment&  Env();
    void StopPipeline();
    TBool CanPlay();
    void PlayPipeline();
    TBool CanPause();
    void PausePipeline();
    TBool CanHalt();
    void HaltPipeline();
    void AddAttribute(const TChar* aAttribute); // FIXME - only required by Songcasting driver
    virtual void RunWithSemaphore(Net::CpStack& aCpStack);
    Media::PipelineManager& Pipeline();
    Net::DvDeviceStandard* Device();
protected:
    virtual void RegisterPlugins(Environment& aEnv);
    void DoRegisterPlugins(Environment& aEnv, const Brx& aSupportedProtocols);
private: // from Net::IResourceManager
    void WriteResource(const Brx& aUriTail, TIpAddress aInterface,
                       std::vector<char*>& aLanguageList,
                       Net::IResourceWriter& aResourceWriter) override;
private:
    static TUint Hash(const Brx& aBuf);
    TBool TryDisable(Net::DvDevice& aDevice);
    void Disabled();
protected:
    MediaPlayer* iMediaPlayer;
    Media::PipelineInitParams* iInitParams;
    Net::DvDeviceStandard* iDevice;
    Net::DvDevice* iDeviceUpnpAv;
    RamStore* iRamStore;
    Configuration::ConfigRegStore* iConfigRegStore;
    Semaphore iSemShutdown;
private:
    Semaphore iDisabled;
    Media::VolumePrinter   iVolume;
    Media::EPipelineState  pState;
    TBool                  iLive;
    ControlPointProxy     *cpProxy;
    const Brx& iUserAgent;
    HWND                   _Hwnd; // Main window handle

private: // from Media::IPipelineObserver
    void NotifyPipelineState(Media::EPipelineState aState) override;
    void NotifyTrack(Media::Track& aTrack, const Brx& aMode, TBool aStartOfStream) override;
    void NotifyMetaText(const Brx& aText) override;
    void NotifyTime(TUint aSeconds, TUint aTrackDurationSeconds) override;
    void NotifyStreamInfo(const Media::DecodedStreamInfo& aStreamInfo) override;
};

class ExampleMediaPlayerInit
{
public:
    static OpenHome::Net::Library* CreateLibrary(TIpAddress preferredSubnet);
};

} // namespace Av
} // namespace OpenHome
