#include "UriProviderAlsaCapture.h"
#include <OpenHome/Types.h>
#include <OpenHome/Media/Filler.h>
#include <OpenHome/Private/Parser.h>
#include <OpenHome/Private/Thread.h>

using namespace OpenHome;
using namespace OpenHome::AlsaCapture;
using namespace OpenHome::Media;

const Brn UriProviderAlsaCapture::kCommandUri("uri");

UriProviderAlsaCapture::UriProviderAlsaCapture(TrackFactory& aTrackFactory)
    : UriProvider("ALSA_CAPTURE",
                  Latency::NotSupported,
                  Pause::NotSupported,
                  Next::NotSupported,
                  Prev::NotSupported,
                  Repeat::NotSupported,
                  Random::NotSupported,
                  RampPauseResume::Long,
                  RampSkip::Short)
    , iTrackFactory(aTrackFactory)
    , iLock("upsd")
    , iTrack(nullptr)
    , iCanPlay(ePlayNo)
{
}

UriProviderAlsaCapture::~UriProviderAlsaCapture()
{
    if (iTrack) {
        iTrack->RemoveRef();
    }
}

void UriProviderAlsaCapture::Reset()
{
    AutoMutex _(iLock);
    if (iTrack) {
        iTrack->RemoveRef();
        iTrack = nullptr;
    }
}

void UriProviderAlsaCapture::Begin(TUint aTrackId)
{
    if (iTrack == nullptr || aTrackId != iTrack->Id()) {
        iCanPlay = ePlayNo;
    }
    else {
        iCanPlay = ePlayYes;
    }
}

void UriProviderAlsaCapture::BeginLater(TUint aTrackId)
{
    if (iTrack == nullptr || aTrackId != iTrack->Id()) {
        iCanPlay = ePlayNo;
    }
    else {
        iCanPlay = ePlayLater;
    }
}

EStreamPlay UriProviderAlsaCapture::GetNext(Track*& aTrack)
{
    AutoMutex _(iLock);
    aTrack = iTrack; // transfer ownership of any reference
    iTrack = nullptr;
    return (aTrack == nullptr) ? ePlayNo : iCanPlay;
}

TUint UriProviderAlsaCapture::CurrentTrackId() const
{
    AutoMutex _(iLock);
    return (iTrack == nullptr) ? Track::kIdNone : iTrack->Id();
}

void UriProviderAlsaCapture::MoveNext()
{
}

void UriProviderAlsaCapture::MovePrevious()
{
}

void UriProviderAlsaCapture::MoveTo(const Brx& aCommand)
{
    if (!aCommand.BeginsWith(kCommandUri)) {
        THROW(FillerInvalidCommand);
    }

    Parser parser(aCommand);
    (void)parser.Next('=');
    Brn uri = parser.Remaining();
    auto track = iTrackFactory.CreateTrack(uri, Brx::Empty());

    AutoMutex _(iLock);
    if (iTrack != nullptr) {
        iTrack->RemoveRef();
    }
    iTrack = track;
    iCanPlay = ePlayYes;
}