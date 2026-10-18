#include "TestMediaPlayer.h"
#include <OpenHome/Types.h>
#include <OpenHome/Media/Tests/Cdecl.h>
#include <OpenHome/Media/Tests/GetCh.h>
#include <OpenHome/Net/Private/DviStack.h>
#include <OpenHome/Media/Utils/AnimatorBasic.h>
#include <OpenHome/Media/PipelineManager.h>
#include <OpenHome/Private/Thread.h>
#include <OpenHome/Functor.h>
#include <memory>
#include <stdlib.h>

using namespace OpenHome;
using namespace OpenHome::Av;
using namespace OpenHome::Av::Test;
using namespace OpenHome::Net;

class TestMediaPlayerThread
{
public:
    TestMediaPlayerThread(const TestMediaPlayerOptions& aOptions);
    ~TestMediaPlayerThread();
    void Run();
private:
    void RunInThread();
private:
    const TestMediaPlayerOptions& iOptions;
    Library* iLib;
    Media::PriorityArbitratorAnimator iArbAnimator;
    Media::PriorityArbitratorPipeline iArbPipeline;
};

TestMediaPlayerThread::TestMediaPlayerThread(const TestMediaPlayerOptions& aOptions)
    : iOptions(aOptions)
    , iArbAnimator(kPrioritySystemHighest)
    , iArbPipeline(kPrioritySystemHighest-1)
{
    iLib = TestMediaPlayerInit::CreateLibrary(iOptions.Room().CString(), iOptions.Loopback().Value(), iOptions.Adapter().Value(), iOptions.Shell().Value());
    ThreadPriorityArbitrator& priorityArbitrator = iLib->Env().PriorityArbitrator();
    priorityArbitrator.Add(iArbAnimator);
    priorityArbitrator.Add(iArbPipeline);
}

TestMediaPlayerThread::~TestMediaPlayerThread()
{
    delete iLib;
}

void TestMediaPlayerThread::Run()
{
    auto th = std::make_unique<ThreadFunctor>("TestMediaPlayer", MakeFunctor(*this, &TestMediaPlayerThread::RunInThread));
    th->Start();
}

void TestMediaPlayerThread::RunInThread()
{
    const TChar* cookie = "TestMediaPlayerMain";
    NetworkAdapter* adapter = iLib->CurrentSubnetAdapter(cookie);
    CpStack* cpStack = nullptr;
    DvStack* dvStack = nullptr;
    iLib->StartCombined(adapter->Subnet(), cpStack, dvStack);

    // Seed random number generator.
    TestMediaPlayerInit::SeedRandomNumberGenerator(dvStack->Env(), iOptions.Room().Value(), adapter->Address(), dvStack->ServerUpnp());
    adapter->RemoveRef(cookie);

    // Set/construct UDN.
    Bwh udn;
    // Note: prefix udn with 4c494e4e- to get older versions of Linn Konfig to recognise our devices
    TestMediaPlayerInit::AppendUniqueId(dvStack->Env(), iOptions.Udn().Value(), Brn("TestMediaPlayer"), udn);
    Log::Print("UDN is %.*s\n", PBUF(udn));

    Log::Print("%s:%d - %s\n", __FILE__, __LINE__, iOptions.Room().CString());
    Log::Print("%s:%d - %s\n", __FILE__, __LINE__, iOptions.Name().CString());
    Log::Print("%s:%d - %s\n", __FILE__, __LINE__, iOptions.TuneIn().CString());
    Log::Print("%s:%d - %s\n", __FILE__, __LINE__, iOptions.Tidal().CString());
    Log::Print("%s:%d - %s\n", __FILE__, __LINE__, iOptions.Qobuz().CString());
    Log::Print("%s:%d - %s\n", __FILE__, __LINE__, iOptions.UserAgent().CString());
    Log::Print("%s:%d - %s\n", __FILE__, __LINE__, iOptions.StoreFile().CString());
    Log::Print("%s:%d - %s\n", __FILE__, __LINE__, iOptions.OptionOdp().Value());
    Log::Print("%s:%d - %s\n", __FILE__, __LINE__, iOptions.OptionWebUi().Value());

    // Create TestMediaPlayer.
    TestMediaPlayer* player = new TestMediaPlayer(*dvStack, *cpStack, udn, iOptions.Room().CString(), iOptions.Name().CString(),
        iOptions.TuneIn().Value(), iOptions.Tidal().Value(), iOptions.Qobuz().Value(),
        iOptions.UserAgent().Value(), iOptions.StoreFile().CString(), iOptions.OptionOdp().Value(), iOptions.OptionWebUi().Value());
    Log::Print("%s:%d\n", __FILE__, __LINE__);
    Log::Print("%s:%d\n", __FILE__, __LINE__);
    player->Run();
    Log::Print("%s:%d\n", __FILE__, __LINE__);
    player->StopPipeline();
    Log::Print("%s:%d\n", __FILE__, __LINE__);
    delete player;
}

int CDECL main(int aArgc, char* aArgv[])
{
    // Parse options.
    TestMediaPlayerOptions options;
    if (!options.Parse(aArgc, aArgv)) {
        return 1;
    }

    auto th = std::make_unique<TestMediaPlayerThread>(options);
    th->Run();

    return 0;
}


