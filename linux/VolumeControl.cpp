#include "VolumeControl.h"

#include <OpenHome/Media/Pipeline/Msg.h>
#include <OpenHome/Av/VolumeManager.h>
#include <OpenHome/Private/Printer.h>

#include <alsa/asoundlib.h>
#include <math.h>


namespace OpenHome {
namespace Av {
using namespace OpenHome::Media;

class VolumeControl::VolumeControlPimpl : public IVolume, public IBalance, public IFade
{
public:
    VolumeControlPimpl(const TChar* aCard, const std::vector<Brn>& aMixerNames);
    ~VolumeControlPimpl();

    TBool IsVolumeSupported() { return iElem != nullptr; }
    void SetVolume(TUint aVolume) override;
    void SetBalance(TInt aBalance) override;
    void SetFade(TInt aFade) override;

private:
    void UpdateHardwareChannels();
    long VolumeToAlsaValue(double aVolumeNormalized);

    snd_mixer_t      *iHandle{nullptr}; 
    snd_mixer_elem_t *iElem{nullptr};   
    TUint             iCurrentVolume{0}; // 0 to 100000+ millidB
    TInt              iCurrentBalance{0}; // Usually scaled negative to positive
    TInt              iCurrentFade{0};    // Usually scaled negative to positive
};

VolumeControl::VolumeControlPimpl::VolumeControlPimpl(const TChar* aCard, const std::vector<Brn>& aMixerNames)
{
    Log::Print("Initializing ALSA Volume Mixer on card: %s\n", aCard);
    
    if (snd_mixer_open(&iHandle, 0) < 0) return;
    
    if (snd_mixer_attach(iHandle, aCard) < 0 ||
        snd_mixer_selem_register(iHandle, NULL, NULL) < 0 ||
        snd_mixer_load(iHandle) < 0) 
    {
        snd_mixer_close(iHandle);
        iHandle = nullptr;
        return;
    }

    snd_mixer_selem_id_t *iSid;
    snd_mixer_selem_id_alloca(&iSid);
    snd_mixer_selem_id_set_index(iSid, 0);

    for (const auto& name : aMixerNames)
    {
        Brhz devName(name);

        snd_mixer_selem_id_set_name(iSid, devName.CString());
        iElem = snd_mixer_find_selem(iHandle, iSid);
        if (iElem != nullptr) {
            Log::Print("VolumeControl: Found working mixer element: %s\n", devName.CString());
            break;
        }
    }
    
    if (iElem == nullptr) {
        Log::Print("VolumeControl: Warning - No matching hardware mixer control found!\n");
    }
}

VolumeControl::VolumeControlPimpl::~VolumeControlPimpl()
{
    if (iHandle) {
        snd_mixer_close(iHandle);
    }
}

long VolumeControl::VolumeControlPimpl::VolumeToAlsaValue(double aVolumeNormalized)
{
    long min, max;
    TInt err = snd_mixer_selem_get_playback_dB_range(iElem, &min, &max);

    if (err < 0 || min >= max) {
        snd_mixer_selem_get_playback_volume_range(iElem, &min, &max);
        return lrint(floor(aVolumeNormalized * (max - min))) + min;
    }

    // Standard linear scale handling if narrow dB range
    if (max - min <= 2400) {
        return lrint(floor(aVolumeNormalized * (max - min))) + min;
    }

    // Logarithmic curve matching human ear perception
    if (min != SND_CTL_TLV_DB_GAIN_MUTE) {
        double min_norm = std::pow(10.0, (min - max) / 6000.0);
        aVolumeNormalized = aVolumeNormalized * (1 - min_norm) + min_norm;
    }
    return lrint(floor(6000.0 * log10(aVolumeNormalized))) + max;
}

void VolumeControl::VolumeControlPimpl::UpdateHardwareChannels()
{
    if (!IsVolumeSupported()) return;

    // Convert raw OpenHome milli-dB steps down to basic percentage scaling (0.0 to 1.0)
    double baseVolume = double((iCurrentVolume / 1024) / 100.0f);
    if (baseVolume > 1.0) baseVolume = 1.0;

    // Handle Balance Modifications (Assuming standard -10 to +10 range from OpenHome)
    double leftScaler = 1.0;
    double rightScaler = 1.0;
    if (iCurrentBalance > 0) leftScaler  -= (iCurrentBalance / 10.0);
    if (iCurrentBalance < 0) rightScaler -= (abs(iCurrentBalance) / 10.0);

    // Handle Fade Modifications (Assuming standard -10 to +10 range, Front vs Rear)
    double frontScaler = 1.0;
    double rearScaler = 1.0;
    if (iCurrentFade > 0) rearScaler  -= (iCurrentFade / 10.0);
    if (iCurrentFade < 0) frontScaler -= (abs(iCurrentFade) / 10.0);

    // Final channel volume mixing calculations
    long leftFrontVal  = VolumeToAlsaValue(baseVolume * leftScaler * frontScaler);
    long rightFrontVal = VolumeToAlsaValue(baseVolume * rightScaler * frontScaler);
    long leftRearVal   = VolumeToAlsaValue(baseVolume * leftScaler * rearScaler);
    long rightRearVal  = VolumeToAlsaValue(baseVolume * rightScaler * rearScaler);

    // Commit parameters safely back to ALSA hardware registers
    snd_mixer_selem_set_playback_volume(iElem, SND_MIXER_SCHN_FRONT_LEFT, leftFrontVal);
    snd_mixer_selem_set_playback_volume(iElem, SND_MIXER_SCHN_FRONT_RIGHT, rightFrontVal);
    
    // Future-proofing for multi-channel/surround expansion
    if (snd_mixer_selem_has_playback_channel(iElem, SND_MIXER_SCHN_REAR_LEFT)) {
        snd_mixer_selem_set_playback_volume(iElem, SND_MIXER_SCHN_REAR_LEFT, leftRearVal);
        snd_mixer_selem_set_playback_volume(iElem, SND_MIXER_SCHN_REAR_RIGHT, rightRearVal);
    }
}

void VolumeControl::VolumeControlPimpl::SetVolume(TUint aVolume)
{
    iCurrentVolume = aVolume;
    UpdateHardwareChannels();
}

void VolumeControl::VolumeControlPimpl::SetBalance(TInt aBalance)
{
    iCurrentBalance = aBalance;
    UpdateHardwareChannels();
}

void VolumeControl::VolumeControlPimpl::SetFade(TInt aFade)
{
    iCurrentFade = aFade;
    UpdateHardwareChannels();
}

VolumeControl::VolumeControl(TBool aDisableVolume, const TChar* aCard, const std::vector<Brn>& aMixerNames)
: iVolumeDisabled(aDisableVolume)
{
    if (iVolumeDisabled) {
        Log::Print("VolumeControl: Software volume disabled via configuration.\n");
        return; 
    }
    impl = std::make_unique<VolumeControlPimpl>(aCard, aMixerNames);
}

VolumeControl::~VolumeControl()
{}

void VolumeControl::SetVolume(TUint aVolume)
{
    if (iVolumeDisabled || !impl) return;
    impl->SetVolume(aVolume);
}

void VolumeControl::SetBalance(TInt aBalance)
{
    if (iVolumeDisabled || !impl) return;
    impl->SetBalance(aBalance);
}

void VolumeControl::SetFade(TInt aFade)
{
    if (iVolumeDisabled || !impl) return;
    impl->SetFade(aFade);
}
}
}
