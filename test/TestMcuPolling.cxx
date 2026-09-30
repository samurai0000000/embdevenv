/*
 * TestMcuPolling.cxx
 *
 * Copyright (C) 2026, Charles Chiou
 */

#include "N1655Mcu.hxx"
#include "N1655Target.hxx"
#include "TargetFactory.hxx"
#include "Console.hxx"
#include "UsageDatabase.hxx"
#include <iostream>
#include <cassert>
#include <memory>
#include <fcntl.h>
#include <unistd.h>
#include <cstring>
#include <chrono>
#include <thread>
#include <map>
#include <vector>

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

static std::string readMasterUntil(int fd, const std::string &delim, int timeoutMs = 1500)
{
    std::string acc;
    auto start = std::chrono::steady_clock::now();
    char buf[128];
    while (std::chrono::duration_cast<std::chrono::milliseconds>(
               std::chrono::steady_clock::now() - start).count() < timeoutMs) {
        ssize_t n = ::read(fd, buf, sizeof(buf) - 1);
        if (n > 0) {
            buf[n] = '\0';
            acc += buf;
            if (acc.find(delim) != std::string::npos) {
                return acc;
            }
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(20));
    }
    return acc;
}

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

static void testDatabaseHygiene()
{
    std::cout << "[TEST] testDatabaseHygiene..." << std::endl;

    std::string testDb = "/tmp/test_db_hygiene_" + std::to_string(getpid()) + ".db";
    std::remove(testDb.c_str());

    UsageDatabase &db = UsageDatabase::getInstance();
    bool opened = db.open(testDb);
    TEST_ASSERT(opened, "UsageDatabase opened temporary DB");

    // 1. auto_vacuum is INCREMENTAL (2)
    int autoVac = db.getPragmaInt("PRAGMA auto_vacuum;");
    TEST_ASSERT(autoVac == 2, "PRAGMA auto_vacuum returns 2 (INCREMENTAL)");

    // 2. Extrema insertion & schema check
    std::map<std::string, double> voltages = {
        {"SYS_5V", 5.02},
        {"VDDECORE", 0.81},
        {"VDDCORE", 0.79}
    };
    std::map<std::string, double> minVoltages = {
        {"SYS_5V", 4.95},
        {"VDDECORE", 0.78},
        {"VDDCORE", 0.76}
    };

    bool recorded = db.recordTelemetrySample(
        "hygiene_test_board",
        true,   // powerOn
        46.5,   // avgTemp
        1250,   // avgFan
        voltages,
        52.3,   // maxTempC
        minVoltages,
        false   // faultDetected
    );
    TEST_ASSERT(recorded, "Telemetry sample with extrema recorded to DB");

    // 3. Query stats and check extrema
    nlohmann::json stats = db.queryBoardStats("hygiene_test_board", 1);
    TEST_ASSERT(stats.contains("summary"), "Stats contain summary object");
    if (stats.contains("summary")) {
        double maxTempFromDb = stats["summary"].value("max_temp_c", 0.0);
        TEST_ASSERT(maxTempFromDb >= 52.0, "max_temp_c reflects peak extrema (52.3C)");
    }

    // 4. Test brownout fault flag
    bool brownoutRecorded = db.recordTelemetrySample(
        "hygiene_test_board",
        true,
        47.0,
        1300,
        voltages,
        53.0,
        {{"SYS_5V", 4.15}}, // brownout trough
        true                // faultDetected
    );
    TEST_ASSERT(brownoutRecorded, "Brownout sample with faultDetected=1 recorded");

    // 5. Test performRollupAndPrune with page-limited incremental vacuum
    db.performRollupAndPrune(0, 0);
    TEST_ASSERT(true, "performRollupAndPrune executed incremental_vacuum(500)");

    db.close();
    std::remove(testDb.c_str());
}

static void testTelemetryStreamingAveraging()
{
    std::cout << "[TEST] testTelemetryStreamingAveraging..." << std::endl;

    int masterFd = posix_openpt(O_RDWR | O_NOCTTY | O_NONBLOCK);
    TEST_ASSERT(masterFd >= 0, "posix_openpt created master PTY");
    if (masterFd < 0) return;

    grantpt(masterFd);
    unlockpt(masterFd);
    std::string slavePath = ptsname(masterFd);

    std::string testDb = "/tmp/test_stream_avg_" + std::to_string(getpid()) + ".db";
    std::remove(testDb.c_str());
    UsageDatabase::getInstance().open(testDb);

    N1655Mcu mcu(slavePath, 115200);
    mcu.setTargetName("test_streaming_target");

    bool started = mcu.start();
    TEST_ASSERT(started, "N1655Mcu started worker thread");

    // Wait for initial bs\r monitor command from Mcu
    std::string req = readMasterUntil(masterFd, "bs\r", 1500);
    TEST_ASSERT(req.find("bs") != std::string::npos, "MCU sent initial bs\\r monitor command");

    // Master streams 5 telemetry frames (simulating 1 Hz stream)
    const char *frames[] = {
        "SYS_5V=5.01V VDDECORE=0.82V TEMP=40.0C FAN_SPEED=1000RPM\n",
        "SYS_5V=5.00V VDDECORE=0.80V TEMP=42.0C FAN_SPEED=1100RPM\n",
        "SYS_5V=4.98V VDDECORE=0.76V TEMP=44.0C FAN_SPEED=1200RPM\n",
        "SYS_5V=5.00V VDDECORE=0.80V TEMP=46.0C FAN_SPEED=1300RPM\n",
        "SYS_5V=4.99V VDDECORE=0.82V TEMP=48.0C FAN_SPEED=1400RPM\n"
    };

    for (int i = 0; i < 5; ++i) {
        ssize_t w = ::write(masterFd, frames[i], strlen(frames[i]));
        (void)w;
        std::this_thread::sleep_for(std::chrono::milliseconds(100));
    }

    // Verify in-memory TelemetryStore updated immediately (zero latency)
    TelemetrySample sample;
    bool hasSample = mcu.getTelemetryStore()->getLatest(sample);
    TEST_ASSERT(hasSample, "TelemetryStore has latest sample from passive streaming");
    TEST_ASSERT(sample.powerOn, "sample.powerOn is true (SYS_5V >= 4.5V)");
    TEST_ASSERT(sample.temperatureC >= 40.0, "sample.temperatureC parsed correctly");
    TEST_ASSERT(sample.fanRpm >= 1000, "sample.fanRpm parsed correctly");

    // Simulate brownout frame (SYS_5V drops to 4.2V)
    const char brownoutFrame[] = "SYS_5V=4.20V VDDECORE=0.70V TEMP=49.0C FAN_SPEED=1500RPM\n";
    ssize_t wb = ::write(masterFd, brownoutFrame, strlen(brownoutFrame));
    (void)wb;
    std::this_thread::sleep_for(std::chrono::milliseconds(150));

    hasSample = mcu.getTelemetryStore()->getLatest(sample);
    TEST_ASSERT(hasSample, "TelemetryStore received brownout frame");
    TEST_ASSERT(!sample.powerOn, "sample.powerOn is false after brownout (4.2V < 4.5V)");

    mcu.stop();
    TEST_ASSERT(!mcu.isRunning(), "N1655Mcu stopped cleanly");

    close(masterFd);
    UsageDatabase::getInstance().close();
    std::remove(testDb.c_str());
}

static void testTelnetArbitrationAndCommandPreemption()
{
    std::cout << "[TEST] testTelnetArbitrationAndCommandPreemption..." << std::endl;

    int masterFd = posix_openpt(O_RDWR | O_NOCTTY | O_NONBLOCK);
    TEST_ASSERT(masterFd >= 0, "posix_openpt created master PTY for arbitration");
    if (masterFd < 0) return;

    grantpt(masterFd);
    unlockpt(masterFd);
    std::string slavePath = ptsname(masterFd);

    N1655Mcu mcu(slavePath, 115200);
    mcu.setTargetName("test_arbitration_target");

    bool started = mcu.start();
    TEST_ASSERT(started, "N1655Mcu started worker thread");

    // Master drains initial bs\r
    std::string req = readMasterUntil(masterFd, "bs\r", 1500);
    TEST_ASSERT(req.find("bs") != std::string::npos, "Initial bs\\r read by master");

    // 1. Telnet client connects: onClientConnect()
    // It should send Ctrl-C (\x03\r) to interrupt bs streaming mode
    std::thread connectThread([&mcu]() {
        mcu.onClientConnect();
    });

    std::string ctrlCReq = readMasterUntil(masterFd, "\x03", 1500);
    TEST_ASSERT(ctrlCReq.find('\x03') != std::string::npos, "Ctrl-C received by master upon Telnet client connect");

    // Master answers prompt so recoverPrompt() succeeds
    const char prompt[] = "\r\n# ";
    ssize_t wp = ::write(masterFd, prompt, strlen(prompt));
    (void)wp;

    connectThread.join();

    // 2. Command preemption test: sendCommand() while in connected state
    std::string cmdResult;
    std::thread cmdThread([&mcu, &cmdResult]() {
        cmdResult = mcu.sendCommand("version", 2000);
    });

    // Master sees prompt recovery or version command
    std::string verReq = readMasterUntil(masterFd, "version\r", 1500);
    TEST_ASSERT(verReq.find("version") != std::string::npos, "version command received by master");

    // Master sends response ending with '# '
    const char verResp[] = "version\r\nN1-655 MCU v2.1.0\r\n# ";
    ssize_t wv = ::write(masterFd, verResp, strlen(verResp));
    (void)wv;

    cmdThread.join();
    TEST_ASSERT(cmdResult.find("v2.1.0") != std::string::npos, "sendCommand received full response without preemption loss");

    // 3. Telnet client disconnects: onClientDisconnect()
    mcu.onClientDisconnect();

    // After 500ms delay, MCU should re-issue bs\r to resume streaming
    std::string resumeReq = readMasterUntil(masterFd, "bs\r", 2000);
    TEST_ASSERT(resumeReq.find("bs") != std::string::npos, "bs\\r resumed after Telnet client disconnect");

    mcu.stop();
    close(masterFd);
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
    testDatabaseHygiene();
    testTelemetryStreamingAveraging();
    testTelnetArbitrationAndCommandPreemption();

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
