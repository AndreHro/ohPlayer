#include "SourceAlsaCapture.h"
#include "UriProviderAlsaCapture.h"
#include "ProtocolAlsaCapture.h"

#include <OpenHome/Types.h>
#include <OpenHome/Buffer.h>
#include <OpenHome/Functor.h>
#include <OpenHome/Av/Source.h>
#include <OpenHome/Av/MediaPlayer.h>
#include <OpenHome/Av/SourceFactory.h>
#include <OpenHome/Media/PipelineManager.h>
#include <OpenHome/Private/Parser.h>
#include <memory> // Added for clean RAII ownership verification pointers

using namespace OpenHome;
using namespace OpenHome::Media;
using namespace OpenHome::Av;

namespace OpenHome {
namespace AlsaCapture {

const Brn SourceAlsaCapture::kSourceNameAlsaCapture("AlsaCapture");
const TChar* SourceAlsaCapture::kSourceTypeAlsaCapture = "AlsaCapture";
const bool SourceAlsaCapture::kDefaultVisibility = true; 

SourceAlsaCapture::SourceAlsaCapture(Av::IMediaPlayer& aMediaPlayer)
    : Source(kSourceNameAlsaCapture,
             kSourceTypeAlsaCapture,
             aMediaPlayer.Pipeline(),
             kDefaultVisibility)
    , iConnected(true)
    , iUriProvider(nullptr)
{
    auto& trackFactory = aMediaPlayer.TrackFactory();

    try {
        iUriProvider = new UriProviderAlsaCapture(trackFactory);
        iUriProvider->SetTransportPlay(MakeFunctor(*this, &SourceAlsaCapture::Play));
        aMediaPlayer.Add(iUriProvider); // Media player takes absolute ownership
    }
    catch (...) {
        // If UriProvider registration fails, iUriProvider was never safely stored 
        // down inside the mediaPlayer context, so we must clean it up locally.
        delete iUriProvider;
        iUriProvider = nullptr;
        throw;
    }
}

SourceAlsaCapture::~SourceAlsaCapture()
{
    // Note: We DO NOT delete protocol or iUriProvider here. 
    // OpenHome's Pipeline and IMediaPlayer frameworks take ownership upon registration 
    // and handle destruction automatically during system teardown phases.
}

void SourceAlsaCapture::Activate(TBool aAutoPlay, TBool /*aPrefetchAllowed*/)
{
    iUriProvider->Reset();
    iPipeline.RemoveAll();

    iPipeline.Begin(iUriProvider->Mode(), iUriProvider->CurrentTrackId());

    if (aAutoPlay) {
        Play();
    }
}

TBool SourceAlsaCapture::TryActivateNoPrefetch(const Brx& aMode)
{
    if (iUriProvider->Mode() != aMode) {
        return false;
    }
    EnsureActiveNoPrefetch();
    return true;
}

void SourceAlsaCapture::PipelineStopped() {}

void SourceAlsaCapture::StandbyEnabled()
{
    iPipeline.Stop();
}

void SourceAlsaCapture::Play()
{
    if (iConnected.load()) {
        iPipeline.Play();
    }
}

} // namespace AlsaCapture
} // namespace OpenHome
