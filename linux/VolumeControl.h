#pragma once

#include <OpenHome/Av/VolumeManager.h>

namespace OpenHome {
namespace Av {

class VolumeControl : public IVolume, public IBalance, public IFade
{
public:
    ~VolumeControl();
    VolumeControl(TBool aDisableVolume, const TChar* aCard, const std::vector<Brn>& aMixerNames); 
private: // from IVolume
    void SetVolume(TUint aVolume) override;
private: // from IBalance
    void SetBalance(TInt aBalance) override;
private: // from IFade
    void SetFade(TInt aFade) override;

private:
    TBool iVolumeDisabled;
    class VolumeControlPimpl;
    std::unique_ptr<VolumeControlPimpl> impl;
};

} // namespace Av
} // namespace OpenHome
