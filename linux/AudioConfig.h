#pragma once

#include <OpenHome/Types.h>
#include <OpenHome/Buffer.h>
#include <OpenHome/Configuration/ConfigManager.h>
#include <OpenHome/Configuration/IStore.h>
#include <memory>

namespace OpenHome {
namespace Av {

class AudioConfig
{
public:
    static const Brn kKeyOutputDevice;
    static const Brn kKeyCaptureDevice;
    static const Brn kKeyCaptureSampleRate;
    static const Brn kKeyKfrOutputRate;
    static const Brn kKeyKfrIrDirectory;

    static const TUint kMaxAlsaDeviceBytes = 63;
    static const TUint kMaxKfrIrDirectoryBytes = 511;
    static const TUint kMinCaptureSampleRate = 8000;
    static const TUint kMaxCaptureSampleRate = 384000;
    static const TUint kDefaultCaptureSampleRate = 48000;
    static const TUint kMaxKfrOutputRate = 768000;

    AudioConfig(Configuration::IConfigInitialiser& aConfigInit);
    ~AudioConfig();

    static void ReadOutputDeviceFromStore(Configuration::IStoreReadWrite& aStore, Bwx& aValue);
    static void ReadCaptureDeviceFromStore(Configuration::IStoreReadWrite& aStore, Bwx& aValue);
    static TUint ReadCaptureSampleRateFromStore(Configuration::IStoreReadWrite& aStore);
    static TUint ReadKfrOutputRateFromStore(Configuration::IStoreReadWrite& aStore);
    static void ReadKfrIrDirectoryFromStore(Configuration::IStoreReadWrite& aStore, Bwx& aValue);

private:
    static void ReadTextFromStore(Configuration::IStoreReadWrite& aStore,
                                  const Brx& aKey, Bwx& aValue, const Brx& aDefault);
    static TUint ReadNumFromStore(Configuration::IStoreReadWrite& aStore,
                                  const Brx& aKey, TUint aMin, TUint aMax, TUint aDefault);

    std::unique_ptr<Configuration::ConfigText> iConfigOutputDevice;
    std::unique_ptr<Configuration::ConfigText> iConfigCaptureDevice;
    std::unique_ptr<Configuration::ConfigNum> iConfigCaptureSampleRate;
    std::unique_ptr<Configuration::ConfigNum> iConfigKfrOutputRate;
    std::unique_ptr<Configuration::ConfigText> iConfigKfrIrDirectory;
};

} // namespace Av
} // namespace OpenHome