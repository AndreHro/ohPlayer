//#include "OpenHomePlayer.h"
#include "TestMediaPlayer.h"
#include "RadioNowPlayingTest.h"
#include "OhLog.h"
#include <OpenHome/Private/Network.h> // SslContext
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
    
    // --test-radio-meta: probe now-playing endpoints of test radio stations and exit
    if (iOptions.TestRadioMeta().Value()) {
        SslContext ssl;
        Av::TestRadioNowPlaying(iLib->Env(), ssl, iOptions.UserAgent().Value());
        adapter->RemoveRef(cookie);
        return;
    }


    // Seed random number generator.
    TestMediaPlayerInit::SeedRandomNumberGenerator(dvStack->Env(), iOptions.Room().Value(), adapter->Address(), dvStack->ServerUpnp());
    adapter->RemoveRef(cookie);

    // The device UDN is derived in the TestMediaPlayer constructor: an
    // explicit -u/--udn is used as provided, otherwise a canonical uuid is
    // derived from the effective room (configured Product.Room, falling
    // back to the -r argument).

    OhLog::Print("Room: %s, Name: %s\n", iOptions.Room().CString(), iOptions.Name().CString());
    OhLog::Print("TuneIn: %s, Tidal: %s, Qobuz: %s\n",
        iOptions.TuneIn().CString(), iOptions.Tidal().CString(), iOptions.Qobuz().CString());
    OhLog::Print("UserAgent: %s, StoreFile: %s\n",
        iOptions.UserAgent().CString(), iOptions.StoreFile().CString());
    OhLog::Print("ODP port: %u, Web UI port: %u\n",
        iOptions.OptionOdp().Value(), iOptions.OptionWebUi().Value());

    // Create TestMediaPlayer.
    TestMediaPlayer* player = new TestMediaPlayer(*dvStack, *cpStack, iOptions.Udn().Value(), iOptions.Room().CString(), iOptions.Name().CString(),
        iOptions.TuneIn().Value(), iOptions.Tidal().Value(), iOptions.Qobuz().Value(),
        iOptions.UserAgent().Value(), iOptions.StoreFile().CString(), iOptions.KfrConfigDir().Value(),
        iOptions.OptionOdp().Value(), iOptions.OptionWebUi().Value());
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


