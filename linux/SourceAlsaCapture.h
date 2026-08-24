#pragma once

#include <OpenHome/Types.h>
#include <OpenHome/Av/Source.h>
#include <OpenHome/Av/MediaPlayer.h>
#include "UriProviderAlsaCapture.h"

namespace OpenHome {
namespace AlsaCapture {

class SourceAlsaCapture : public Av::Source
{
public:
    SourceAlsaCapture(Av::IMediaPlayer& aMediaPlayer);
    ~SourceAlsaCapture() override;

private: // from ISource
    void Activate(TBool aAutoPlay, TBool aPrefetchAllowed) override;
    TBool TryActivateNoPrefetch(const Brx& aMode) override;
    void PipelineStopped() override;
    void StandbyEnabled() override;

private:
    void Play();

private:
    std::atomic<bool>       iConnected;
    UriProviderAlsaCapture* iUriProvider;
   

    static const Brn        kSourceNameAlsaCapture;
    static const TChar*     kSourceTypeAlsaCapture;
    static const bool       kDefaultVisibility;
};

} // namespace AlsaCapture
} // namespace OpenHome
