#include "AudioConfig.h"
#include <OpenHome/Private/Converter.h>

namespace OpenHome {
namespace Av {

const Brn AudioConfig::kKeyOutputDevice("Audio.OutputDevice");
const Brn AudioConfig::kKeyCaptureDevice("Audio.CaptureDevice");
const Brn AudioConfig::kKeyCaptureSampleRate("Audio.CaptureSampleRate");
const Brn AudioConfig::kKeyKfrOutputRate("Audio.KfrOutputRate");
const Brn AudioConfig::kKeyKfrIrDirectory("Audio.KfrIrDirectory");

const TUint AudioConfig::kMaxAlsaDeviceBytes;
const TUint AudioConfig::kMaxKfrIrDirectoryBytes;
const TUint AudioConfig::kMinCaptureSampleRate;
const TUint AudioConfig::kMaxCaptureSampleRate;
const TUint AudioConfig::kDefaultCaptureSampleRate;
const TUint AudioConfig::kMaxKfrOutputRate;

AudioConfig::AudioConfig(Configuration::IConfigInitialiser& aConfigInit)
{
    iConfigOutputDevice = std::make_unique<Configuration::ConfigText>(
        aConfigInit, kKeyOutputDevice, 1, kMaxAlsaDeviceBytes,
        Brn("hw:CARD=Standa,DEV=0"), true);
    iConfigCaptureDevice = std::make_unique<Configuration::ConfigText>(
        aConfigInit, kKeyCaptureDevice, 1, kMaxAlsaDeviceBytes,
        Brn("hw:CARD=Audio,DEV=0"), true);
    iConfigCaptureSampleRate = std::make_unique<Configuration::ConfigNum>(
        aConfigInit, kKeyCaptureSampleRate, kMinCaptureSampleRate,
        kMaxCaptureSampleRate, kDefaultCaptureSampleRate, true);
    iConfigKfrOutputRate = std::make_unique<Configuration::ConfigNum>(
        aConfigInit, kKeyKfrOutputRate, 0, kMaxKfrOutputRate, 0, true);
    iConfigKfrIrDirectory = std::make_unique<Configuration::ConfigText>(
        aConfigInit, kKeyKfrIrDirectory, 0, kMaxKfrIrDirectoryBytes,
        Brx::Empty(), true);
}

AudioConfig::~AudioConfig()
{
}

void AudioConfig::ReadOutputDeviceFromStore(Configuration::IStoreReadWrite& aStore, Bwx& aValue)
{
    ReadTextFromStore(aStore, kKeyOutputDevice, aValue, Brn("hw:CARD=Standa,DEV=0"));
}

void AudioConfig::ReadCaptureDeviceFromStore(Configuration::IStoreReadWrite& aStore, Bwx& aValue)
{
    ReadTextFromStore(aStore, kKeyCaptureDevice, aValue, Brn("hw:CARD=Audio,DEV=0"));
}

TUint AudioConfig::ReadCaptureSampleRateFromStore(Configuration::IStoreReadWrite& aStore)
{
    return ReadNumFromStore(aStore, kKeyCaptureSampleRate, kMinCaptureSampleRate,
                            kMaxCaptureSampleRate, kDefaultCaptureSampleRate);
}

TUint AudioConfig::ReadKfrOutputRateFromStore(Configuration::IStoreReadWrite& aStore)
{
    return ReadNumFromStore(aStore, kKeyKfrOutputRate, 0, kMaxKfrOutputRate, 0);
}

void AudioConfig::ReadKfrIrDirectoryFromStore(Configuration::IStoreReadWrite& aStore, Bwx& aValue)
{
    ReadTextFromStore(aStore, kKeyKfrIrDirectory, aValue, Brx::Empty());
}

void AudioConfig::ReadTextFromStore(Configuration::IStoreReadWrite& aStore,
                                    const Brx& aKey, Bwx& aValue, const Brx& aDefault)
{
    try {
        aStore.Read(aKey, aValue);
        if (aValue.Bytes() == 0 && aDefault.Bytes() != 0) {
            aValue.Replace(aDefault);
        }
    }
    catch (StoreKeyNotFound&) {
        aValue.Replace(aDefault);
    }
}

TUint AudioConfig::ReadNumFromStore(Configuration::IStoreReadWrite& aStore,
                                    const Brx& aKey, TUint aMin, TUint aMax, TUint aDefault)
{
    Bws<sizeof(TInt)> buf;
    try {
        aStore.Read(aKey, buf);
        const TInt val = static_cast<TInt>(Converter::BeUint32At(buf, 0));
        if (val >= static_cast<TInt>(aMin) && val <= static_cast<TInt>(aMax)) {
            return static_cast<TUint>(val);
        }
    }
    catch (StoreKeyNotFound&) {
    }
    return aDefault;
}

} // namespace Av
} // namespace OpenHome