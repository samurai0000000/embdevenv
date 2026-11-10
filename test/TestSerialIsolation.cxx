/*
 * TestSerialIsolation.cxx
 *
 * Copyright (C) 2026, Charles Chiou
 */

#include "SerialPort.hxx"
#include "Console.hxx"
#include "N1655Mcu.hxx"
#include <iostream>
#include <fstream>
#include <filesystem>
#include <string>
#include <vector>
#include <cstdlib>
#include <fcntl.h>
#include <unistd.h>
#include <cstring>
#include <chrono>
#include <thread>

using namespace embdevenv;
namespace fs = std::filesystem;

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

struct MockSysfsFixture {
    fs::path rootPath;
    fs::path classTty;
    fs::path usbDevices;

    MockSysfsFixture() {
        char tempDir[] = "/tmp/embdevenv_mock_sysfs_XXXXXX";
        char *created = mkdtemp(tempDir);
        if (created) {
            rootPath = created;
            classTty = rootPath / "sys" / "class" / "tty";
            usbDevices = rootPath / "sys" / "devices";
            fs::create_directories(classTty);
            fs::create_directories(usbDevices);
        }
    }

    ~MockSysfsFixture() {
        if (!rootPath.empty() && fs::exists(rootPath)) {
            std::error_code ec;
            fs::remove_all(rootPath, ec);
        }
    }

    void addCh9344Node(const std::string &name, int minor, const std::string &usbPath) {
        fs::path ttyDir = classTty / name;
        fs::create_directories(ttyDir);

        // Create dev file with major:minor
        std::ofstream devFile(ttyDir / "dev");
        devFile << "188:" << minor << "\n";
        devFile.close();

        // Create target USB device interface directory
        fs::path usbDevIface = usbDevices / ("pci0000:00/0000:00:14.0/usb4/4-10/" + usbPath + "/" + usbPath + ":1.0");
        fs::create_directories(usbDevIface);

        // Symlink device -> usbDevIface
        std::error_code ec;
        fs::create_directory_symlink(usbDevIface, ttyDir / "device", ec);
    }

    void addGenericUsbNode(const std::string &name, int minor, const std::string &bInterfaceNumber, const std::string &usbPath) {
        fs::path ttyDir = classTty / name;
        fs::create_directories(ttyDir);

        std::ofstream devFile(ttyDir / "dev");
        devFile << "188:" << minor << "\n";
        devFile.close();

        fs::path usbDevIface = usbDevices / ("pci0000:00/0000:00:14.0/usb1/" + usbPath + "/" + usbPath + ":1." + bInterfaceNumber);
        fs::create_directories(usbDevIface);

        std::ofstream ifaceFile(usbDevIface / "bInterfaceNumber");
        ifaceFile << bInterfaceNumber << "\n";
        ifaceFile.close();

        std::error_code ec;
        fs::create_directory_symlink(usbDevIface, ttyDir / "device", ec);
    }
};

static void testCh9344MissingNodesIsolation()
{
    std::cout << "[TEST] testCh9344MissingNodesIsolation..." << std::endl;

    MockSysfsFixture fixture;
    TEST_ASSERT(!fixture.rootPath.empty(), "Mock sysfs directory created");

    // Simulate scenario where nodes 8, 9, 10 are dead/missing, only node 11 exists
    // Node 11 has minor 11. 11 % 8 == 3 (Channel 3 / MCU).
    fixture.addCh9344Node("ttyCH9344USB11", 11, "4-10.3.2");

    std::string sysfsTty = fixture.classTty.string();

    std::string mcuNode = SerialPort::resolveUsbPortNode("4-10.3.2", 3, sysfsTty);
    TEST_ASSERT(mcuNode == "/dev/ttyCH9344USB11",
                "Channel 3 resolves to ttyCH9344USB11 even when channels 0-2 are missing");

    std::string ch0Node = SerialPort::resolveUsbPortNode("4-10.3.2", 0, sysfsTty);
    TEST_ASSERT(ch0Node.empty(),
                "Channel 0 returns empty string when node 8 is absent");

    std::string ch1Node = SerialPort::resolveUsbPortNode("4-10.3.2", 1, sysfsTty);
    TEST_ASSERT(ch1Node.empty(),
                "Channel 1 returns empty string when node 9 is absent");

    std::string ch2Node = SerialPort::resolveUsbPortNode("4-10.3.2", 2, sysfsTty);
    TEST_ASSERT(ch2Node.empty(),
                "Channel 2 returns empty string when node 10 is absent");
}

static void testCh9344AliasingRegression()
{
    std::cout << "[TEST] testCh9344AliasingRegression..." << std::endl;

    MockSysfsFixture fixture;
    // Node 12 has minor 12.
    // In buggy % 4 arithmetic: 12 % 4 == 0 (would collide with Channel 0!).
    // In correct % 8 arithmetic: 12 % 8 == 4 (Channel 4).
    fixture.addCh9344Node("ttyCH9344USB12", 12, "4-10.3.2");

    std::string sysfsTty = fixture.classTty.string();

    std::string ch0Node = SerialPort::resolveUsbPortNode("4-10.3.2", 0, sysfsTty);
    TEST_ASSERT(ch0Node.empty(),
                "Channel 0 does NOT falsely match minor 12 (aliasing prevention)");

    std::string ch4Node = SerialPort::resolveUsbPortNode("4-10.3.2", 4, sysfsTty);
    TEST_ASSERT(ch4Node == "/dev/ttyCH9344USB12",
                "Channel 4 matches minor 12 (12 % 8 == 4)");
}

static void testGenericUsbInterfaceNumberResolution()
{
    std::cout << "[TEST] testGenericUsbInterfaceNumberResolution..." << std::endl;

    MockSysfsFixture fixture;
    // Generic USB serial with non-contiguous minor numbers
    fixture.addGenericUsbNode("ttyUSB5", 5, "00", "1-2.3");
    fixture.addGenericUsbNode("ttyUSB9", 9, "01", "1-2.3");
    fixture.addGenericUsbNode("ttyUSB2", 2, "03", "1-2.3");

    std::string sysfsTty = fixture.classTty.string();

    std::string ch0 = SerialPort::resolveUsbPortNode("1-2.3", 0, sysfsTty);
    TEST_ASSERT(ch0 == "/dev/ttyUSB5",
                "Generic USB: Channel 0 resolved by bInterfaceNumber 00 (ttyUSB5)");

    std::string ch1 = SerialPort::resolveUsbPortNode("1-2.3", 1, sysfsTty);
    TEST_ASSERT(ch1 == "/dev/ttyUSB9",
                "Generic USB: Channel 1 resolved by bInterfaceNumber 01 (ttyUSB9)");

    std::string ch2 = SerialPort::resolveUsbPortNode("1-2.3", 2, sysfsTty);
    TEST_ASSERT(ch2.empty(),
                "Generic USB: Channel 2 missing returns empty");

    std::string ch3 = SerialPort::resolveUsbPortNode("1-2.3", 3, sysfsTty);
    TEST_ASSERT(ch3 == "/dev/ttyUSB2",
                "Generic USB: Channel 3 resolved by bInterfaceNumber 03 (ttyUSB2)");
}

static void testLiveHostSysfsCh9344()
{
    std::cout << "[TEST] testLiveHostSysfsCh9344..." << std::endl;

    if ((fs::exists("/sys/class/tty/ttyCH9344USB0") &&
         fs::exists("/sys/class/tty/ttyCH9344USB3")) ||
        (fs::exists("/sys/class/tty/ttyCH9344USB8") &&
         fs::exists("/sys/class/tty/ttyCH9344USB11"))) {
        std::string mcu = SerialPort::resolveUsbPortNode("4-10.3.2", 3);
        std::string c0 = SerialPort::resolveUsbPortNode("4-10.3.2", 0);
        bool validPair = (c0 == "/dev/ttyCH9344USB0" && mcu == "/dev/ttyCH9344USB3") ||
                         (c0 == "/dev/ttyCH9344USB8" && mcu == "/dev/ttyCH9344USB11");
        TEST_ASSERT(validPair && !mcu.empty(), "Live host 4-10.3.2 MCU resolved to channel 3 (USB3 or USB11)");
        TEST_ASSERT(validPair && !c0.empty(), "Live host 4-10.3.2 Console 0 resolved to channel 0 (USB0 or USB8)");
    } else {
        std::cout << "  (Skipping live test: CH9344 hardware not attached on host)" << std::endl;
    }
}

static void testConsoleSilentLinePreservation()
{
    std::cout << "[TEST] testConsoleSilentLinePreservation..." << std::endl;

    int masterFd = posix_openpt(O_RDWR | O_NOCTTY);
    TEST_ASSERT(masterFd >= 0, "posix_openpt created master PTY");
    if (masterFd < 0) return;

    grantpt(masterFd);
    unlockpt(masterFd);
    const char *pts = ptsname(masterFd);
    TEST_ASSERT(pts != nullptr, "ptsname returned slave name");
    if (!pts) {
        close(masterFd);
        return;
    }
    std::string slavePath = pts;

    SerialPort port(slavePath, 115200);
    bool opened = port.open();
    TEST_ASSERT(opened, "SerialPort opened slave PTY");

    // Test silent line: read with 50ms timeout when master wrote nothing
    char buf[128];
    ssize_t n = port.read(buf, sizeof(buf), 50);
    TEST_ASSERT(n == 0, "SerialPort read returned 0 on silent line");
    TEST_ASSERT(port.isOpen(), "SerialPort remains open on silent line (FD not closed)");

    // Test data transmission after silence
    const char msg[] = "test_payload\n";
    ssize_t w = ::write(masterFd, msg, strlen(msg));
    TEST_ASSERT(w == static_cast<ssize_t>(strlen(msg)), "Master wrote data");

    n = port.read(buf, sizeof(buf), 200);
    TEST_ASSERT(n > 0, "SerialPort captured incoming data after silent period");
    if (n > 0) {
        buf[n] = '\0';
        TEST_ASSERT(std::string(buf).find("test_payload") != std::string::npos,
                    "Payload verified");
    }

    port.close();
    close(masterFd);
}

static void testConsoleSelfHealingTransitions()
{
    std::cout << "[TEST] testConsoleSelfHealingTransitions..." << std::endl;

    int masterFd = posix_openpt(O_RDWR | O_NOCTTY);
    TEST_ASSERT(masterFd >= 0, "posix_openpt created master PTY for Console");
    if (masterFd < 0) return;

    grantpt(masterFd);
    unlockpt(masterFd);
    std::string slavePath = ptsname(masterFd);

    Console console("test_cons", slavePath, 115200);
    TEST_ASSERT(!console.isRunning(), "Console not running initially");

    bool started = console.start();
    TEST_ASSERT(started, "Console started worker thread");

    // Wait briefly for connection
    std::this_thread::sleep_for(std::chrono::milliseconds(50));
    TEST_ASSERT(console.isConnected(), "Console connected to slave PTY");

    // Send lines through master
    const char line1[] = "booting kernel...\n";
    ssize_t w = ::write(masterFd, line1, strlen(line1));
    (void)w;
    std::this_thread::sleep_for(std::chrono::milliseconds(100));

    auto lines = console.getRecentLines();
    bool found = false;
    for (const auto &l : lines) {
        if (l.find("booting kernel") != std::string::npos) {
            found = true;
            break;
        }
    }
    TEST_ASSERT(found, "Console received line through active worker");

    // Stop console
    console.stop();
    TEST_ASSERT(!console.isRunning(), "Console stopped cleanly");
    close(masterFd);
}

static void testMcuPowerStateAndAdaptiveUsbBoot()
{
    std::cout << "[TEST] testMcuPowerStateAndAdaptiveUsbBoot..." << std::endl;

    int masterFd = posix_openpt(O_RDWR | O_NOCTTY | O_NONBLOCK);
    TEST_ASSERT(masterFd >= 0, "posix_openpt created master PTY for Mcu");
    if (masterFd < 0) return;

    grantpt(masterFd);
    unlockpt(masterFd);
    std::string slavePath = ptsname(masterFd);

    N1655Mcu mcu(slavePath, 115200, 15000);

    // 1. Initial state: No telemetry sample exists
    TEST_ASSERT(mcu.getPowerState() == Mcu::PowerState::UNKNOWN,
                "Initial MCU power state is UNKNOWN");

    // UNKNOWN state must REJECT USB boot
    bool bootResult = mcu.resetUsbDownload(false);
    TEST_ASSERT(!bootResult, "resetUsbDownload rejected when power state is UNKNOWN");

    // Verify no commands were sent to master
    char buf[256];
    ssize_t n = ::read(masterFd, buf, sizeof(buf));
    TEST_ASSERT(n <= 0, "No command sent to UART during UNKNOWN state rejection");

    // Launch responder thread to supply '#' prompt to any commands
    std::atomic<bool> responderRunning{true};
    std::string capturedCmds;
    std::thread responder([masterFd, &responderRunning, &capturedCmds]() {
        char inBuf[128];
        while (responderRunning.load()) {
            ssize_t r = ::read(masterFd, inBuf, sizeof(inBuf) - 1);
            if (r > 0) {
                inBuf[r] = '\0';
                capturedCmds += inBuf;
                // reply with prompt
                const char prompt[] = "\r\n# ";
                ssize_t pw = ::write(masterFd, prompt, strlen(prompt));
                (void)pw;
            }
            std::this_thread::sleep_for(std::chrono::milliseconds(10));
        }
    });

    // 2. Power OFF state
    TelemetrySample offSample;
    offSample.powerOn = false;
    offSample.temperatureC = 25.0;
    offSample.voltages["SYS_5V"] = 0.0;
    mcu.getTelemetryStore()->addSample(offSample);

    TEST_ASSERT(mcu.getPowerState() == Mcu::PowerState::OFF,
                "MCU power state is OFF when sample.powerOn == false");

    // Execute resetUsbDownload under OFF state -> must execute "pwr on -usb"
    bool offBootResult = mcu.resetUsbDownload(false);
    TEST_ASSERT(offBootResult, "resetUsbDownload succeeded under OFF state");
    TEST_ASSERT(capturedCmds.find("pwr on -usb") != std::string::npos,
                "pwr on -usb was dispatched when power state is OFF");

    // 3. Power ON state
    capturedCmds.clear();
    TelemetrySample onSample;
    onSample.powerOn = true;
    onSample.temperatureC = 45.0;
    onSample.voltages["SYS_5V"] = 5.05;
    mcu.getTelemetryStore()->addSample(onSample);

    TEST_ASSERT(mcu.getPowerState() == Mcu::PowerState::ON,
                "MCU power state is ON when sample.powerOn == true");

    bool onBootResult = mcu.resetUsbDownload(false);
    TEST_ASSERT(onBootResult, "resetUsbDownload succeeded under ON state");
    TEST_ASSERT(capturedCmds.find("reset -usb -y") != std::string::npos,
                "reset -usb -y was dispatched when power state is ON");

    responderRunning.store(false);
    if (responder.joinable()) {
        responder.join();
    }
    close(masterFd);
}

static void testMcuStatusJsonTriState()
{
    std::cout << "[TEST] testMcuStatusJsonTriState..." << std::endl;

    N1655Mcu mcu("/dev/null", 115200, 15000);

    auto j1 = mcu.getStatusJson();
    TEST_ASSERT(j1["powerState"] == "unknown", "getStatusJson reports powerState 'unknown' initially");
    TEST_ASSERT(j1["powerOn"] == false, "powerOn is false when powerState is unknown");

    TelemetrySample offSample;
    offSample.powerOn = false;
    mcu.getTelemetryStore()->addSample(offSample);
    auto j2 = mcu.getStatusJson();
    TEST_ASSERT(j2["powerState"] == "off", "getStatusJson reports powerState 'off'");
    TEST_ASSERT(j2["powerOn"] == false, "powerOn is false when powerState is off");

    TelemetrySample onSample;
    onSample.powerOn = true;
    mcu.getTelemetryStore()->addSample(onSample);
    auto j3 = mcu.getStatusJson();
    TEST_ASSERT(j3["powerState"] == "on", "getStatusJson reports powerState 'on'");
    TEST_ASSERT(j3["powerOn"] == true, "powerOn is true when powerState is on");
}

static void testConsolePromptCursorIsolation()
{
    std::cout << "[TEST] testConsolePromptCursorIsolation..." << std::endl;

    int masterFd = posix_openpt(O_RDWR | O_NOCTTY);
    TEST_ASSERT(masterFd >= 0, "posix_openpt succeeded");
    grantpt(masterFd);
    unlockpt(masterFd);
    std::string slavePath = ptsname(masterFd);

    Console cons("test_prompt", slavePath, 115200, 6098);
    cons.start();

    // Give thread time to open
    std::this_thread::sleep_for(std::chrono::milliseconds(50));

    // 1. Write completed line
    const char line1[] = "Linux version 6.1.112\r\n";
    ssize_t w1 = ::write(masterFd, line1, strlen(line1));
    (void)w1;
    std::this_thread::sleep_for(std::chrono::milliseconds(50));

    auto completed1 = cons.getCompletedLines(100);
    std::string partial1 = cons.getPartialLine();
    TEST_ASSERT(completed1.size() == 1, "Completed lines has 1 entry");
    if (!completed1.empty()) {
        TEST_ASSERT(completed1[0] == "Linux version 6.1.112", "Completed line matches exact content");
    }
    TEST_ASSERT(partial1.empty(), "Partial prompt line is empty after complete line");

    // 2. Write prompt without newline (e.g. localhost login: )
    const char prompt[] = "localhost login: ";
    ssize_t w2 = ::write(masterFd, prompt, strlen(prompt));
    (void)w2;
    std::this_thread::sleep_for(std::chrono::milliseconds(50));

    auto completed2 = cons.getCompletedLines(100);
    std::string partial2 = cons.getPartialLine();
    TEST_ASSERT(completed2.size() == 1, "Completed lines still has only 1 entry");
    TEST_ASSERT(partial2 == "localhost login: ", "Partial line contains prompt without trailing newline");

    // getRecentLines() backwards-compatible returns 2
    auto recent2 = cons.getRecentLines(100);
    TEST_ASSERT(recent2.size() == 2, "getRecentLines returns completed lines + partial prompt");

    // 3. User hits Enter (target echoes \r\n and prints next prompt)
    const char enterPrompt[] = "\r\nlocalhost login: ";
    ssize_t w3 = ::write(masterFd, enterPrompt, strlen(enterPrompt));
    (void)w3;
    std::this_thread::sleep_for(std::chrono::milliseconds(50));

    auto completed3 = cons.getCompletedLines(100);
    std::string partial3 = cons.getPartialLine();
    TEST_ASSERT(completed3.size() == 2, "Completed lines now has 2 entries");
    if (completed3.size() >= 2) {
        TEST_ASSERT(completed3[1] == "localhost login: ", "Previous prompt was committed as completed line");
    }
    TEST_ASSERT(partial3 == "localhost login: ", "New prompt is isolated in partial line");

    cons.stop();
    close(masterFd);
}

static void testFilesystemBrowseApi()
{
    std::cout << "[TEST] testFilesystemBrowseApi..." << std::endl;

    fs::path tempDir = fs::temp_directory_path() / ("test_fs_browse_" + std::to_string(getpid()));
    fs::create_directories(tempDir / "subdir_a");
    fs::create_directories(tempDir / "subdir_b");

    std::ofstream(tempDir / "firmware.bin") << "dummy binary";
    std::ofstream(tempDir / "package.pkg") << "dummy package";
    std::ofstream(tempDir / "readme.txt") << "text file";
    std::ofstream(tempDir / ".hidden_file") << "hidden";

    std::vector<std::string> exts = {".bin", ".pkg"};
    std::error_code ec;

    struct EntryInfo {
        std::string name;
        bool isDir;
        int64_t size;
    };
    std::vector<EntryInfo> list;

    for (const auto &entry : fs::directory_iterator(tempDir, fs::directory_options::skip_permission_denied, ec)) {
        if (ec) continue;
        bool isDir = fs::is_directory(entry.status(ec));
        std::string filename = entry.path().filename().string();
        if (filename.empty() || filename[0] == '.') continue;

        if (!isDir && !exts.empty()) {
            std::string entryExt = entry.path().extension().string();
            bool match = false;
            for (const auto &e : exts) {
                if (entryExt == e) { match = true; break; }
            }
            if (!match) continue;
        }

        int64_t sz = isDir ? 0 : static_cast<int64_t>(entry.file_size(ec));
        list.push_back({filename, isDir, sz});
    }

    std::sort(list.begin(), list.end(), [](const EntryInfo &a, const EntryInfo &b) {
        if (a.isDir != b.isDir) return a.isDir > b.isDir;
        return a.name < b.name;
    });

    TEST_ASSERT(list.size() == 4, "Browse returned 4 items (2 dirs, 2 matching files)");
    if (list.size() >= 4) {
        TEST_ASSERT(list[0].isDir && list[0].name == "subdir_a", "First item is subdir_a");
        TEST_ASSERT(list[1].isDir && list[1].name == "subdir_b", "Second item is subdir_b");
        TEST_ASSERT(!list[2].isDir && list[2].name == "firmware.bin", "Third item is firmware.bin");
        TEST_ASSERT(!list[3].isDir && list[3].name == "package.pkg", "Fourth item is package.pkg");
    }

    fs::remove_all(tempDir);
}

int main(int argc, char **argv)
{
    (void)argc;
    (void)argv;

    std::cout << "========================================" << std::endl;
    std::cout << " Running SerialIsolation Unit Tests" << std::endl;
    std::cout << "========================================" << std::endl;

    testCh9344MissingNodesIsolation();
    testCh9344AliasingRegression();
    testGenericUsbInterfaceNumberResolution();
    testLiveHostSysfsCh9344();
    testConsoleSilentLinePreservation();
    testConsoleSelfHealingTransitions();
    testMcuPowerStateAndAdaptiveUsbBoot();
    testMcuStatusJsonTriState();
    testConsolePromptCursorIsolation();
    testFilesystemBrowseApi();

    std::cout << "----------------------------------------" << std::endl;
    std::cout << " Tests Passed: " << g_testsPassed << std::endl;
    std::cout << " Tests Failed: " << g_testsFailed << std::endl;
    std::cout << "========================================" << std::endl;

    return (g_testsFailed == 0) ? 0 : 1;
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
