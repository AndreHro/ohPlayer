#pragma once

#include <OpenHome/Types.h>
#include <OpenHome/Media/Filler.h>
#include <OpenHome/Private/Thread.h>

namespace OpenHome {
namespace AlsaCapture {

class UriProviderAlsaCapture : public Media::UriProvider
{
    static const Brn kCommandUri;
public:
    UriProviderAlsaCapture(Media::TrackFactory& aTrackFactory);
    ~UriProviderAlsaCapture();
    void Reset();
    void MoveTo(const Brx& aCommand) override;
    TUint CurrentTrackId() const override;

private: // UriProvider
    void Begin(TUint aTrackId) override;
    void BeginLater(TUint aTrackId) override;
    Media::EStreamPlay GetNext(Media::Track*& aTrack) override;
    
    void MoveNext() override;
    void MovePrevious() override;
    
private:
    Media::TrackFactory& iTrackFactory;
    mutable Mutex iLock;
    Media::Track* iTrack;
    Media::EStreamPlay iCanPlay;
};

} // namespace AlsaCapture
} // namespace OpenHome