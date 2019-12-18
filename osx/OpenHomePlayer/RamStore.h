#ifndef HEADER_RAM_STORE
#define HEADER_RAM_STORE

#include <OpenHome/Av/KvpStore.h>

namespace OpenHome {
namespace Av {

/**
 * Helper class for test code.  Not intended for client use.
 */
class RamStore : public IStaticDataSource
{
public:
    RamStore(const Brx& aImageFileName);
    virtual ~RamStore();
private: // from IStaticDataSource
    void LoadStaticData(IStoreLoaderStatic& aLoader) override;
private:
    Brhz iImageFileName;
};

} // namespace Av
} // namespace OpenHome

#endif // HEADER_RAM_STORE
