/*
 * TestMcuPolling.cxx
 *
 * Copyright (C) 2026, Charles Chiou
 */

#include "N1655Mcu.hxx"
#include "N1655Target.hxx"
#include "TargetFactory.hxx"
#include "Console.hxx"
#include <iostream>
#include <cassert>
#include <memory>

using namespace embdevenv;

static int g_testsPassed = 0;
static int g_testsFailed = 0;

#define TEST_ASSERT(cond, msg) \
    do { \
        if (!(cond)) { \
            std::cerr << "FAIL: " << msg << " (" << __FILE__ << ":" << __LINE__ << ")" << std::endl; \
            g_testsFailed++; \
        } else { \
            g_testsPassed++; \
        } \
    } while (0)

static void testPollingPauseResume()
{
    std::cout << "[TEST] testPollingPauseResume..." << std::endl;

    N1655Mcu mcu("/dev/null", 115200, 15000);
    TEST_ASSERT(!mcu.isPollingPaused(), "Initially polling is not paused");
    TEST_ASSERT(mcu.getPollIntervalMs() == 15000, "Default poll interval is 15000ms");

    mcu.pausePolling();
    TEST_ASSERT(mcu.isPollingPaused(), "Polling is paused after 1 pause");

    mcu.pausePolling();
    TEST_ASSERT(mcu.isPollingPaused(), "Polling is paused after 2 pauses");

    mcu.resumePolling();
    TEST_ASSERT(mcu.isPollingPaused(), "Polling remains paused after 1 resume (ref count = 1)");

    mcu.resumePolling();
    TEST_ASSERT(!mcu.isPollingPaused(), "Polling is unpaused after 2nd resume (ref count = 0)");

    // Extra resume does not underflow
    mcu.resumePolling();
    TEST_ASSERT(!mcu.isPollingPaused(), "Polling remains unpaused on extra resume");
}

static void testScopedPollPause()
{
    std::cout << "[TEST] testScopedPollPause..." << std::endl;

    auto mcu = std::make_shared<N1655Mcu>("/dev/null", 115200, 15000);
    TEST_ASSERT(!mcu->isPollingPaused(), "Initially not paused");

    {
        Mcu::ScopedPollPause pause1(mcu);
        TEST_ASSERT(mcu->isPollingPaused(), "Paused in outer scope");

        {
            Mcu::ScopedPollPause pause2(mcu);
            TEST_ASSERT(mcu->isPollingPaused(), "Paused in nested scope");
        }

        TEST_ASSERT(mcu->isPollingPaused(), "Still paused after inner scope exit");
    }

    TEST_ASSERT(!mcu->isPollingPaused(), "Unpaused after outer scope exit");
}

static void testTargetCoordination()
{
    std::cout << "[TEST] testTargetCoordination..." << std::endl;

    auto target = std::make_shared<Target>("test-target", "n1-655", "Test Target", true);
    auto mcu = std::make_shared<N1655Mcu>("/dev/null", 115200, 15000);
    auto console = std::make_shared<Console>("console0", "/dev/null", 115200);

    target->setMcu(mcu);
    target->addConsole(console);

    TEST_ASSERT(!target->isMcuPollingPaused(), "Target MCU initially not paused");

    {
        Target::ScopedMcuPollPause targetPause(target);
        TEST_ASSERT(target->isMcuPollingPaused(), "Target MCU paused via ScopedMcuPollPause");
        TEST_ASSERT(mcu->isPollingPaused(), "Underlying MCU is paused");
    }

    TEST_ASSERT(!target->isMcuPollingPaused(), "Target MCU unpaused after scope");
    TEST_ASSERT(!mcu->isPollingPaused(), "Underlying MCU is unpaused");
}

static void testFactoryDefaults()
{
    std::cout << "[TEST] testFactoryDefaults..." << std::endl;

    TargetConfig cfg;
    cfg.name = "default-test";
    cfg.model = "n1-655-pro";
    cfg.mcu.port = "/dev/null";

    TEST_ASSERT(cfg.mcu.pollIntervalMs == 15000, "McuConfig default interval is 15000ms");
    TEST_ASSERT(cfg.mcuPollIntervalMs == 15000, "Legacy default interval is 15000ms");

    auto target = TargetFactory::createTarget(cfg);
    TEST_ASSERT(target != nullptr, "Target created successfully");
    if (target && target->getMcu()) {
        auto n1655 = std::dynamic_pointer_cast<N1655Mcu>(target->getMcu());
        if (n1655) {
            TEST_ASSERT(n1655->getPollIntervalMs() == 15000, "N1655Mcu instantiated with 15000ms");
        }
    }
}

int main()
{
    std::cout << "========================================" << std::endl;
    std::cout << " Running McuPolling Unit Tests" << std::endl;
    std::cout << "========================================" << std::endl;

    testPollingPauseResume();
    testScopedPollPause();
    testTargetCoordination();
    testFactoryDefaults();

    std::cout << "----------------------------------------" << std::endl;
    std::cout << " Tests Passed: " << g_testsPassed << std::endl;
    std::cout << " Tests Failed: " << g_testsFailed << std::endl;
    std::cout << "========================================" << std::endl;

    return g_testsFailed == 0 ? 0 : 1;
}

/*
 * Local variables:
 * mode: C++
 * c-file-style: "BSD"
 * c-basic-offset: 4
 * tab-width: 4
 * indent-tabs-mode: nil
 * End:
 */
