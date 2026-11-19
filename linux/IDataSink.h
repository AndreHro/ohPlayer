#pragma once

#include <OpenHome/Buffer.h>

namespace OpenHome {
namespace Media {

class IDataSink
{
public:
    virtual void Write(const Brx& aData) = 0;
    virtual ~IDataSink() {}
};

} // namespace Media
} // namespace OpenHome
