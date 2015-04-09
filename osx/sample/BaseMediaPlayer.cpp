#include "BaseMediaPlayer.h"
#include <OpenHome/Types.h>
#include <OpenHome/Net/Private/DviStack.h>
#include <OpenHome/Av/MediaPlayer.h>
#include <OpenHome/Av/Product.h>
#include <OpenHome/Net/Core/DvDevice.h>
#include <OpenHome/Media/PipelineManager.h>
#include <OpenHome/Private/Printer.h>
#include <OpenHome/Private/TestFramework.h>
#include <OpenHome/Media/Codec/CodecFactory.h>
#include <OpenHome/Media/Protocol/ProtocolFactory.h>
#include <OpenHome/Av/SourceFactory.h>
#include <OpenHome/Av/KvpStore.h>
#include "RamStore.h"
#include <OpenHome/Av/UpnpAv/UpnpAv.h>
#include <OpenHome/Configuration/ConfigManager.h>
#include <OpenHome/Av/Utils/IconDriverSongcastSender.h>
#include <OpenHome/Net/Private/Shell.h>
#include <OpenHome/Net/Private/ShellCommandDebug.h>
#include <OpenHome/Private/Parser.h>
#include <OpenHome/Media/Debug.h>
#include <OpenHome/Av/Debug.h>
#include <OpenHome/Av/Credentials.h>
#include <OpenHome/Media/Pipeline/Pipeline.h>
#include <OpenHome/Web/WebAppFramework.h>
#include <OpenHome/Web/ConfigUi/ConfigUi.h>

#include "ConfigRamStore.h"

using namespace OpenHome;
using namespace OpenHome::Av;
using namespace OpenHome::Av::Sample;
using namespace OpenHome::Configuration;
using namespace OpenHome::Media;
using namespace OpenHome::Net;
using namespace OpenHome::TestFramework;
using namespace OpenHome::Web;

const Brn BaseMediaPlayer::kSongcastSenderIconFileName("SongcastSenderIcon");

BaseMediaPlayer::BaseMediaPlayer(Net::DvStack& aDvStack, const Brx& aUdn, const TChar* aRoom, const TChar* aProductName,
const Brx& aTuneInPartnerId, const Brx& aTidalId, const Brx& aQobuzIdSecret, const Brx& aUserAgent)
: iSemShutdown("TMPS", 0)
, iDisabled("test", 0)
, iTuneInPartnerId(aTuneInPartnerId)
, iTidalId(aTidalId)
, iQobuzIdSecret(aQobuzIdSecret)
, iUserAgent(aUserAgent)
, iPullableClock(NULL)
, iObservableFriendlyName(new Bws<RaopDevice::kMaxNameBytes>())
{
    Bws<256> friendlyName;
    friendlyName.Append(aRoom);
    friendlyName.Append(':');
    friendlyName.Append(aProductName);

    // create UPnP device
    iDevice = new DvDeviceStandard(aDvStack, aUdn, *this);
    iDevice->SetAttribute("Upnp.Domain", "av.openhome.org");
    iDevice->SetAttribute("Upnp.Type", "Source");
    iDevice->SetAttribute("Upnp.Version", "1");
    iDevice->SetAttribute("Upnp.FriendlyName", friendlyName.PtrZ());
    iDevice->SetAttribute("Upnp.Manufacturer", "OpenHome");
    iDevice->SetAttribute("Upnp.ModelName", "Avarice");

    // create separate UPnP device for standard MediaRenderer
    Bws<256> buf(aUdn);
    buf.Append("-MediaRenderer");
    // The renderer name should be <room name>:<UPnP AV source name> to allow
    // our control point to match the renderer device to the upnp av source.
    //
    // FIXME - will have to allow this to be dynamically changed at runtime if
    // someone changes the name of the UPnP AV source.
    // Disable device -> change name -> re-enable device.
    Bws<256> rendererName(aRoom);
    rendererName.Append(":");
    rendererName.Append(SourceUpnpAv::kSourceName);
    iDeviceUpnpAv = new DvDeviceStandard(aDvStack, buf);
    iDeviceUpnpAv->SetAttribute("Upnp.Domain", "upnp.org");
    iDeviceUpnpAv->SetAttribute("Upnp.Type", "MediaRenderer");
    iDeviceUpnpAv->SetAttribute("Upnp.Version", "1");
    friendlyName.Append(":MediaRenderer");
    iDeviceUpnpAv->SetAttribute("Upnp.FriendlyName", rendererName.PtrZ());
    iDeviceUpnpAv->SetAttribute("Upnp.Manufacturer", "OpenHome");
    iDeviceUpnpAv->SetAttribute("Upnp.ModelName", "BaseMediaPlayer");

    // create read/write store.  This creates a number of static (constant) entries automatically
    // FIXME - to be removed; this only exists to populate static data
    iRamStore = new RamStore();

    // create a read/write store using the new config framework
    iConfigRamStore = new ConfigRamStore();

    // FIXME - available store keys should be listed somewhere
    iConfigRamStore->Write(Brn("Product.Room"), Brn(aRoom));
    iConfigRamStore->Write(Brn("Product.Name"), Brn(aProductName));

    // create MediaPlayer
    iMediaPlayer = new MediaPlayer(aDvStack, *iDevice, *iRamStore, *iConfigRamStore, PipelineInitParams::New(),
    iVolume, iVolume, aUdn, Brn("Main Room"), Brn("Softplayer"));
    iPipelineObserver = new LoggingPipelineObserver();
    iMediaPlayer->Pipeline().AddObserver(*iPipelineObserver);

    // register our PowerDownUpnp function with the PowerManager
    IPowerManager& powerManager = iMediaPlayer->PowerManager();
    iPowerObserver = powerManager.Register(*this, kPowerPriorityLowest);

    // create a shell
    iShell = new Shell(aDvStack.Env(), 0);
    Log::Print("Shell running on port %u\n", iShell->Port());
    iShellDebug = new ShellCommandDebug(*iShell);

    // Set up config app.
    static const TUint addr = 0;    // Bind to all addresses.
    static const TUint port = 8080;    // Bind to whatever free port the OS allocates to the framework server.
    iAppFramework = new WebAppFramework(aDvStack.Env(), addr, port, kMaxUiTabs, kUiSendQueueSize);
}

BaseMediaPlayer::~BaseMediaPlayer()
{
    delete iAppFramework;
    delete iPowerObserver;
    ASSERT(!iDevice->Enabled());
    delete iMediaPlayer;
    delete iPipelineObserver;
    delete iShellDebug;
    delete iShell;
    delete iDevice;
    delete iDeviceUpnpAv;
    delete iRamStore;
    delete iConfigRamStore;
}

void BaseMediaPlayer::SetPullableClock(IPullableClock& aPullableClock)
{
    iPullableClock = &aPullableClock;
}

void BaseMediaPlayer::PausePipeline()
{
    Pipeline().Pause();
}

void BaseMediaPlayer::PlayPipeline()
{
    Pipeline().Play();
}

void BaseMediaPlayer::StopPipeline()
{
    Pipeline().Stop();
    TUint waitCount = 0;
    if (TryDisable(*iDevice)) {
        waitCount++;
    }
    if (TryDisable(*iDeviceUpnpAv)) {
        waitCount++;
    }
    while (waitCount > 0) {
        iDisabled.Wait();
        waitCount--;
    }
    iMediaPlayer->Quit();
    iSemShutdown.Signal();
}

void BaseMediaPlayer::AddAttribute(const TChar* aAttribute)
{
    iMediaPlayer->AddAttribute(aAttribute);
}

void BaseMediaPlayer::Run()
{
    RegisterPlugins(iMediaPlayer->Env());
    iMediaPlayer->Start();

    std::vector<const Brx*> sourcesBufs;
    Product& product = iMediaPlayer->Product();
    for (TUint i=0; i<product.SourceCount(); i++) {
        Bws<ISource::kMaxSystemNameBytes> systemName;
        Bws<ISource::kMaxSourceNameBytes> name;
        Bws<ISource::kMaxSourceTypeBytes> type;
        TBool visible;
        product.GetSourceDetails(i, systemName, type, name, visible);
        sourcesBufs.push_back(new Brh(systemName));
    }
    // FIXME - take resource dir as param or copy res dir to build dir
    iConfigApp = new ConfigAppMediaPlayer(iMediaPlayer->ConfigManager(), sourcesBufs, Brn("Softplayer"), Brn("res/"), kMaxUiTabs, kUiSendQueueSize);
    iAppFramework->Add(iConfigApp, MakeFunctorGeneric(*this, &BaseMediaPlayer::PresentationUrlChanged));

    for (TUint i=0;i<sourcesBufs.size(); i++) {
        delete sourcesBufs[i];
    }

    iAppFramework->Start();
    iDevice->SetEnabled();
    iDeviceUpnpAv->SetEnabled();

}

void BaseMediaPlayer::RunWithSemaphore()
{
    RegisterPlugins(iMediaPlayer->Env());
    iMediaPlayer->Start();
    iAppFramework->Start();
    iDevice->SetEnabled();
    iDeviceUpnpAv->SetEnabled();

    iConfigRamStore->Print();

    iSemShutdown.Wait();    // FIXME - can Run() and RunWithSemaphore() be refactored out? only difference is how they wait for termination signal

    //IPowerManager& powerManager = iMediaPlayer->PowerManager();
    //powerManager.PowerDown(); // FIXME - this should probably be replaced by a normal shutdown procedure
    iConfigRamStore->Print();
}

PipelineManager& BaseMediaPlayer::Pipeline()
{
    return iMediaPlayer->Pipeline();
}

DvDeviceStandard* BaseMediaPlayer::Device()
{
    return iDevice;
}

void BaseMediaPlayer::RegisterPlugins(Environment& aEnv)
{
    const Brn kSupportedProtocols(
    "http-get:*:audio/x-flac:*,"    // Flac
    "http-get:*:audio/wav:*,"       // Wav
    "http-get:*:audio/wave:*,"      // Wav
    "http-get:*:audio/x-wav:*,"     // Wav
    "http-get:*:audio/aiff:*,"      // AIFF
    "http-get:*:audio/x-aiff:*,"    // AIFF
    "http-get:*:audio/x-m4a:*,"     // Alac
    "http-get:*:audio/x-scpls:*,"   // M3u (content processor)
    "http-get:*:text/xml:*,"        // Opml ??  (content processor)
    "http-get:*:audio/aac:*,"       // Aac
    "http-get:*:audio/aacp:*,"      // Aac
    "http-get:*:audio/mp4:*,"       // Mpeg4 (container)
    "http-get:*:audio/ogg:*,"       // Vorbis
    "http-get:*:audio/x-ogg:*,"     // Vorbis
    "http-get:*:application/ogg:*," // Vorbis
    "tidalhifi.com:*:*:*,"          // Tidal
    );
    DoRegisterPlugins(aEnv, kSupportedProtocols);
}

void BaseMediaPlayer::DoRegisterPlugins(Environment& aEnv, const Brx& aSupportedProtocols)
{
    // Add codecs
    iMediaPlayer->Add(Codec::CodecFactory::NewAac());
    iMediaPlayer->Add(Codec::CodecFactory::NewAiff());
    iMediaPlayer->Add(Codec::CodecFactory::NewAifc());
    iMediaPlayer->Add(Codec::CodecFactory::NewAlac());
    iMediaPlayer->Add(Codec::CodecFactory::NewAdts());
    iMediaPlayer->Add(Codec::CodecFactory::NewFlac());
    iMediaPlayer->Add(Codec::CodecFactory::NewPcm());
    iMediaPlayer->Add(Codec::CodecFactory::NewVorbis());
    iMediaPlayer->Add(Codec::CodecFactory::NewWav());

    // Add protocol modules (Radio source can require several stacked Http instances)
    iMediaPlayer->Add(ProtocolFactory::NewHttp(aEnv, iUserAgent));
    iMediaPlayer->Add(ProtocolFactory::NewHttp(aEnv, iUserAgent));
    iMediaPlayer->Add(ProtocolFactory::NewHttp(aEnv, iUserAgent));
    iMediaPlayer->Add(ProtocolFactory::NewHttp(aEnv, iUserAgent));
    iMediaPlayer->Add(ProtocolFactory::NewHttp(aEnv, iUserAgent));
    iMediaPlayer->Add(ProtocolFactory::NewHls(aEnv, iUserAgent));

    // only add Tidal if we have a token to use with login
    if (iTidalId.Bytes() > 0) {
    iMediaPlayer->Add(ProtocolFactory::NewTidal(aEnv, iTidalId, iMediaPlayer->CredentialsManager(), iMediaPlayer->ConfigInitialiser()));
    }
    // ...likewise, only add Qobuz if we have ids for login
    if (iQobuzIdSecret.Bytes() > 0) {
    Parser p(iQobuzIdSecret);
    Brn appId(p.Next(':'));
    Brn appSecret(p.Remaining());
    Log::Print("Qobuz: appId = ");
    Log::Print(appId);
    Log::Print(", appSecret = ");
    Log::Print(appSecret);
    Log::Print("\n");
    iMediaPlayer->Add(ProtocolFactory::NewQobuz(aEnv, appId, appSecret, iMediaPlayer->CredentialsManager(), iMediaPlayer->ConfigInitialiser()));
    }

    // Add sources
    iMediaPlayer->Add(SourceFactory::NewPlaylist(*iMediaPlayer, aSupportedProtocols));
    if (iTuneInPartnerId.Bytes() == 0) {
    iMediaPlayer->Add(SourceFactory::NewRadio(*iMediaPlayer, iPullableClock, aSupportedProtocols));
    }
    else {
    iMediaPlayer->Add(SourceFactory::NewRadio(*iMediaPlayer, iPullableClock, aSupportedProtocols, iTuneInPartnerId));
    }
    iMediaPlayer->Add(SourceFactory::NewUpnpAv(*iMediaPlayer, *iDeviceUpnpAv, aSupportedProtocols));

    Bwh hostName(iDevice->Udn().Bytes()+1); // space for null terminator
    hostName.Replace(iDevice->Udn());
    Bws<12> macAddr;
    MacAddrFromUdn(aEnv, macAddr);
    const TChar* friendlyName;
    iDevice->GetAttribute("Upnp.FriendlyName", &friendlyName);
    iObservableFriendlyName.Replace(Brn(friendlyName));
    iMediaPlayer->Add(SourceFactory::NewRaop(*iMediaPlayer, hostName.PtrZ(), iObservableFriendlyName, macAddr));

    iMediaPlayer->Add(SourceFactory::NewReceiver(*iMediaPlayer, iPullableClock, NULL, kSongcastSenderIconFileName)); // FIXME - will want to replace timestamper with access to a driver on embedded platforms
}



void BaseMediaPlayer::WriteResource(const Brx& aUriTail, TIpAddress /*aInterface*/, std::vector<char*>& /*aLanguageList*/, IResourceWriter& aResourceWriter)
{
    if (aUriTail == kSongcastSenderIconFileName) {
    aResourceWriter.WriteResourceBegin(sizeof(kIconDriverSongcastSender), kIconDriverSongcastSenderMimeType);
    aResourceWriter.WriteResource(kIconDriverSongcastSender, sizeof(kIconDriverSongcastSender));
    aResourceWriter.WriteResourceEnd();
    }
}

void BaseMediaPlayer::PowerUp()
{
    // FIXME - enable UPnP devices here?
    // - would need to account for two-stage create->run process either by
    //  - setting a flag here which is checked in Run() OR
    //  - registering with IPowerManager in Run() call
    //iDevice->SetEnabled();
    //iDeviceUpnpAv->SetEnabled();
}

void BaseMediaPlayer::PowerDown()
{
    Log::Print("BaseMediaPlayer::PowerDown\n");
    PowerDownDisable(*iDevice);
    PowerDownDisable(*iDeviceUpnpAv);
}


TUint BaseMediaPlayer::Hash(const Brx& aBuf)
{
    TUint hash = 0;
    for (TUint i=0; i<aBuf.Bytes(); i++) {
    hash += aBuf[i];
    }
    return hash;
}

void BaseMediaPlayer::GenerateMacAddr(Environment& aEnv, TUint aSeed, Bwx& aMacAddr)
{
    // Generate a 48-bit, 12-byte hex string.
    // Method:
    // - Generate two random numbers in the range 0 - 2^24
    // - Get the hex representation of these numbers
    // - Combine the two hex representations into the output buffer, aMacAddr
    const TUint maxLimit = 0x01000000;
    Bws<8> macBuf1;
    Bws<8> macBuf2;

    aEnv.SetRandomSeed(aSeed);
    TUint mac1 = aEnv.Random(maxLimit, 0);
    TUint mac2 = aEnv.Random(maxLimit, 0);

    Ascii::AppendHex(macBuf1, mac1);
    Ascii::AppendHex(macBuf2, mac2);

    aMacAddr.Append(macBuf1.Split(2));
    aMacAddr.Append(macBuf2.Split(2));
}

void BaseMediaPlayer::MacAddrFromUdn(Environment& aEnv, Bwx& aMacAddr)
{
    TUint hash = Hash(iDevice->Udn());
    GenerateMacAddr(aEnv, hash, aMacAddr);
}

void BaseMediaPlayer::PresentationUrlChanged(const Brx& aUrl)
{
    Bws<Uri::kMaxUriBytes+1> url(aUrl);   // +1 for '\0'
    iDevice->SetAttribute("Upnp.PresentationUrl", url.PtrZ());
}

void BaseMediaPlayer::PowerDownDisable(DvDevice& aDevice)
{
    if (aDevice.Enabled()) {
    aDevice.SetDisabled(MakeFunctor(*this, &BaseMediaPlayer::PowerDownUpnpCallback));
    }
}

void BaseMediaPlayer::PowerDownUpnpCallback()
{
// do nothing; only exists to avoid lengthy Upnp shutdown waits during power fail
}

TBool BaseMediaPlayer::TryDisable(DvDevice& aDevice)
{
    if (aDevice.Enabled()) {
    aDevice.SetDisabled(MakeFunctor(*this, &BaseMediaPlayer::Disabled));
    return true;
    }
    return false;
}

void BaseMediaPlayer::Disabled()
{
    iDisabled.Signal();
}


// BaseMediaPlayerOptions

BaseMediaPlayerOptions::BaseMediaPlayerOptions()
: iOptionRoom("-r", "--room", Brn("OsxRoom"), "room the Product service will report")
, iOptionName("-n", "--name", Brn("SoftPlayer"), "Product name")
, iOptionUdn("-u", "--udn", Brn(""), "Udn (optional - one will be generated if this is left blank)")
, iOptionChannel("-c", "--channel", 0, "[0..65535] sender channel")
, iOptionAdapter("-a", "--adapter", 0, "[adapter] index of network adapter to use")
, iOptionLoopback("-l", "--loopback", "Use loopback adapter")
, iOptionTuneIn("-t", "--tunein", Brn(""), "TuneIn partner id")
, iOptionTidal("", "--tidal", Brn(""), "Tidal token")
, iOptionQobuz("", "--qobuz", Brn(""), "app_id:app_secret")
, iOptionUserAgent("", "--useragent", Brn(""), "User Agent (for HTTP requests)")
{
    iParser.AddOption(&iOptionRoom);
    iParser.AddOption(&iOptionName);
    iParser.AddOption(&iOptionUdn);
    iParser.AddOption(&iOptionChannel);
    iParser.AddOption(&iOptionAdapter);
    iParser.AddOption(&iOptionLoopback);
    iParser.AddOption(&iOptionTuneIn);
    iParser.AddOption(&iOptionTidal);
    iParser.AddOption(&iOptionQobuz);
    iParser.AddOption(&iOptionUserAgent);
}

void BaseMediaPlayerOptions::AddOption(Option* aOption)
{
    iParser.AddOption(aOption);
}

TBool BaseMediaPlayerOptions::Parse(int aArgc, char* aArgv[])
{
    return iParser.Parse(aArgc, aArgv);
}

OptionString& BaseMediaPlayerOptions::Room()
{
    return iOptionRoom;
}

OptionString& BaseMediaPlayerOptions::Name()
{
    return iOptionName;
}

OptionString& BaseMediaPlayerOptions::Udn()
{
    return iOptionUdn;
}

OptionUint& BaseMediaPlayerOptions::Channel()
{
    return iOptionChannel;
}

OptionUint& BaseMediaPlayerOptions::Adapter()
{
    return iOptionAdapter;
}

OptionBool& BaseMediaPlayerOptions::Loopback()
{
    return iOptionLoopback;
}

OptionString& BaseMediaPlayerOptions::TuneIn()
{
    return iOptionTuneIn;
}

OptionString& BaseMediaPlayerOptions::Tidal()
{
    return iOptionTidal;
}

OptionString& BaseMediaPlayerOptions::Qobuz()
{
    return iOptionQobuz;
}

OptionString& BaseMediaPlayerOptions::UserAgent()
{
    return iOptionUserAgent;
}


// BaseMediaPlayerInit

OpenHome::Net::Library* BaseMediaPlayerInit::CreateLibrary(TBool aLoopback, TUint aAdapter)
{
    InitialisationParams* initParams = InitialisationParams::Create();
    initParams->SetDvEnableBonjour();
    if (aLoopback == true) {
        initParams->SetUseLoopbackNetworkAdapter();
    }

    Debug::SetLevel(Debug::kSongcast | Debug::kPipeline);
    Net::Library* lib = new Net::Library(initParams);

    std::vector<NetworkAdapter*>* subnetList = lib->CreateSubnetList();
    const TUint adapterIndex = aAdapter;
    if (subnetList->size() <= adapterIndex) {
        Log::Print("ERROR: adapter %u doesn't exist\n", adapterIndex);
        ASSERTS();
    }
    Log::Print ("adapter list:\n");
    for (unsigned i=0; i<subnetList->size(); ++i) {
        TIpAddress addr = (*subnetList)[i]->Address();
        Log::Print ("  %d: %d.%d.%d.%d\n", i, addr&0xff, (addr>>8)&0xff, (addr>>16)&0xff, (addr>>24)&0xff);
    }

    TIpAddress subnet = (*subnetList)[adapterIndex]->Subnet();
    Library::DestroySubnetList(subnetList);
    lib->SetCurrentSubnet(subnet);
    Log::Print("using subnet %d.%d.%d.%d\n", subnet&0xff, (subnet>>8)&0xff, (subnet>>16)&0xff, (subnet>>24)&0xff);
    return lib;
}

void BaseMediaPlayerInit::SeedRandomNumberGenerator(Environment& aEnv, const Brx& aRoom, TIpAddress aAddress, DviServerUpnp& aServer)
{
    if (aRoom == Brx::Empty()) {
        Log::Print("ERROR: room must be set\n");
        ASSERTS();
    }
    // Re-seed random number generator with hash of (unique) room name + UPnP
    // device server port to avoid UDN clashes.
    TUint port = aServer.Port(aAddress);
    Log::Print("UPnP DV server using port: %u\n", port);
    TUint hash = 0;
    for (TUint i=0; i<aRoom.Bytes(); i++) {
        hash += aRoom[i];
    }
    hash += port;
    Log::Print("Seeding random number generator with: %u\n", hash);
    aEnv.SetRandomSeed(hash);
}

void BaseMediaPlayerInit::AppendUniqueId(Environment& aEnv, const Brx& aUserUdn, const Brx& aDefaultUdn, Bwh& aOutput)
{
    if (aUserUdn.Bytes() == 0) {
        if (aOutput.MaxBytes() < aDefaultUdn.Bytes()) {
            aOutput.Grow(aDefaultUdn.Bytes());
        }
        aOutput.Replace(aDefaultUdn);
        RandomiseUdn(aEnv, aOutput);
    }
    else {
        if (aUserUdn.Bytes() > aOutput.MaxBytes()) {
            aOutput.Grow(aUserUdn.Bytes());
        }
        aOutput.Replace(aUserUdn);
    }
}
