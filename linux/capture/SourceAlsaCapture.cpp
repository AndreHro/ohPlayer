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
const TChar* SourceAlsaCapture::kSourceTypeAlsaCapture = "Digital";
const bool SourceAlsaCapture::kDefaultVisibility = true; 

SourceAlsaCapture::SourceAlsaCapture(Av::IMediaPlayer& aMediaPlayer)
    : Source(kSourceNameAlsaCapture,
             kSourceTypeAlsaCapture,
             aMediaPlayer.Pipeline(),
             kDefaultVisibility)
    , iConnected(true)
    , iStopped(true)
    , iUriProvider(nullptr)
{
    auto& trackFactory = aMediaPlayer.TrackFactory();

    try {
        iUriProvider = new UriProviderAlsaCapture(trackFactory);
        iUriProvider->SetTransportPlay(MakeFunctor(*this, &SourceAlsaCapture::Play));
        iUriProvider->SetTransportPause(MakeFunctor(*this, &SourceAlsaCapture::Pause));
        iUriProvider->SetTransportStop(MakeFunctor(*this, &SourceAlsaCapture::Stop));
        aMediaPlayer.Add(iUriProvider);
    }
    catch (...) {
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

void SourceAlsaCapture::Activate(TBool /*aAutoPlay*/, TBool /*aPrefetchAllowed*/)
{
    iUriProvider->Reset();
    iUriProvider->MoveTo(Brn("uri=alsacapture://default"));
    iPipeline.RemoveAll();

    iPipeline.Begin(iUriProvider->Mode(), iUriProvider->CurrentTrackId());
    iStopped.store(false);
}

void SourceAlsaCapture::Deactivate()
{
    iStopped.store(true);
    Av::Source::Deactivate();
}

TBool SourceAlsaCapture::TryActivateNoPrefetch(const Brx& aMode)
{
    if (iUriProvider->Mode() != aMode) {
        return false;
    }
    EnsureActiveNoPrefetch();
    return true;
}


void SourceAlsaCapture::PipelineStopped()
{
    iStopped.store(true);
}

void SourceAlsaCapture::StandbyEnabled()
{
    Stop();
}

void SourceAlsaCapture::Play()
{
    if (!iConnected.load() || !IsActive()) {
        return;
    }
    if (iStopped.load()) {
        iUriProvider->Reset();
        iUriProvider->MoveTo(Brn("uri=alsacapture://default"));
        iPipeline.RemoveAll();
        iPipeline.Begin(iUriProvider->Mode(), iUriProvider->CurrentTrackId());
        iStopped.store(false);
    }
    iPipeline.Play();
}

void SourceAlsaCapture::Pause()
{
    if (IsActive() && !iStopped.load()) {
        iPipeline.Pause();
    }
}

void SourceAlsaCapture::Stop()
{
    if (IsActive()) {
        iStopped.store(true);
        iPipeline.Stop();
    }
}

} // namespace AlsaCapture
} // namespace OpenHome
