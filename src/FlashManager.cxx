/*
 * FlashManager.cxx
 *
 * Copyright (C) 2026, Charles Chiou
 */

#include "FlashManager.hxx"
#include "UsbMatrixEmbedded.hxx"
#include "Config.hxx"
#include "TargetManager.hxx"
#include "UsageDatabase.hxx"
#include <sys/stat.h>
#include <sys/types.h>
#include <unistd.h>
#include <sys/select.h>
#include <fcntl.h>
#include <fstream>
#include <sstream>
#include <iostream>
#include <regex>
#include <thread>
#include <chrono>
#include <ctime>

namespace embdevenv {

nlohmann::json FlashResult::toJson() const
{
    return {
        {"success", success},
        {"target", target},
        {"stage", stage},
        {"mode", mode},
        {"duration_ms", durationMs},
        {"usb_matrix_log", usbMatrixLog},
        {"error", error},
        {"timestamp", timestampIso}
    };
}

FlashManager &FlashManager::getInstance()
{
    static FlashManager instance;
    return instance;
}

FlashManager::FlashManager()
    : _isFlashing(false)
{
}

std::string FlashManager::ensureUsbMatrixBinary()
{
    std::lock_guard<std::mutex> lock(_mutex);

    if (!_cachedBinaryPath.empty() && access(_cachedBinaryPath.c_str(), X_OK) == 0) {
        return _cachedBinaryPath;
    }

    // 1. Check if user configured an explicit override in Config
    std::string overrideBin = Config::getInstance().getFlashingGlobalConfig().usbMatrixBin;
    if (!overrideBin.empty() && access(overrideBin.c_str(), X_OK) == 0) {
        _cachedBinaryPath = overrideBin;
        return _cachedBinaryPath;
    }

    // 2. Check standard user local directory
    std::string binDir = Config::resolveHomePath("~/.config/embdevenv/bin");
    mkdir(binDir.c_str(), 0755);

    std::string targetPath = binDir + "/usb-matrix";

    struct stat st;
    bool needsExtraction = true;
    if (stat(targetPath.c_str(), &st) == 0) {
        if (static_cast<size_t>(st.st_size) == EMBEDDED_USB_MATRIX_BIN_LEN &&
            access(targetPath.c_str(), X_OK) == 0) {
            needsExtraction = false;
        }
    }

    if (needsExtraction) {
        std::ofstream ofs(targetPath, std::ios::binary | std::ios::trunc);
        if (ofs) {
            ofs.write(reinterpret_cast<const char *>(EMBEDDED_USB_MATRIX_BIN),
                      EMBEDDED_USB_MATRIX_BIN_LEN);
            ofs.close();
            chmod(targetPath.c_str(), 0755);
        } else {
            // Fallback to /tmp if ~/.config cannot be written
            targetPath = "/tmp/embdevenv-usb-matrix";
            std::ofstream ofsTmp(targetPath, std::ios::binary | std::ios::trunc);
            if (ofsTmp) {
                ofsTmp.write(reinterpret_cast<const char *>(EMBEDDED_USB_MATRIX_BIN),
                             EMBEDDED_USB_MATRIX_BIN_LEN);
                ofsTmp.close();
                chmod(targetPath.c_str(), 0755);
            }
        }
    }

    _cachedBinaryPath = targetPath;
    return _cachedBinaryPath;
}

std::string FlashManager::executeCommand(const std::string &cmd, int &exitCode, int timeoutSec, std::function<void(const std::string&)> onOutput)
{
    std::string fullCmd = "timeout -k 5s " + std::to_string(timeoutSec > 0 ? timeoutSec : 60) + " " + cmd + " 2>&1";
    FILE *fp = popen(fullCmd.c_str(), "r");
    if (!fp) {
        exitCode = -1;
        return "Failed to run command: " + cmd;
    }

    int fd = fileno(fp);
    int flags = fcntl(fd, F_GETFL, 0);
    fcntl(fd, F_SETFL, flags | O_NONBLOCK);

    std::string output;
    char buf[512];
    auto startTime = std::chrono::steady_clock::now();
    int timeoutMs = (timeoutSec > 0 ? timeoutSec : 60) * 1000 + 5000;

    while (true) {
        fd_set read_fds;
        FD_ZERO(&read_fds);
        FD_SET(fd, &read_fds);

        struct timeval tv;
        tv.tv_sec = 1;
        tv.tv_usec = 0;

        int ret = select(fd + 1, &read_fds, nullptr, nullptr, &tv);
        if (ret > 0) {
            ssize_t bytesRead = read(fd, buf, sizeof(buf) - 1);
            if (bytesRead > 0) {
                buf[bytesRead] = '\0';
                output += buf;
                if (onOutput) onOutput(output);

                // Detect usb-matrix wait counter underflow / exhaustion
                // (e.g. "remaining 4294967295" or "remaining 0 sec(s)" with 0/1 devices)
                if (output.find("remaining 4294967") != std::string::npos ||
                    (output.find("waiting for devices (0/1)") != std::string::npos &&
                     output.find("remaining 0 sec(s)") != std::string::npos)) {
                    output += "\n[embdevenv] usb-matrix wait-counter underflow/exhaustion detected. Aborting early.\n";
                    if (onOutput) onOutput(output);
                    int sysRet = ::system("pkill -f usb-matrix 2>/dev/null");
                    (void)sysRet;
                    exitCode = 124;
                    break;
                }
            } else if (bytesRead == 0) {
                break; // EOF
            } else {
                if (errno != EAGAIN && errno != EWOULDBLOCK) {
                    break;
                }
            }
        } else if (ret == 0) {
            auto now = std::chrono::steady_clock::now();
            int elapsedMs = std::chrono::duration_cast<std::chrono::milliseconds>(now - startTime).count();
            if (elapsedMs >= timeoutMs) {
                output += "\n[embdevenv] executeCommand pipe read timeout exceeded.\n";
                if (onOutput) onOutput(output);
                break; // Break the blocking read!
            }
        } else {
            if (errno != EINTR) break;
        }
    }

    int status = pclose(fp);
    if (exitCode == 0) {
        if (WIFEXITED(status)) {
            exitCode = WEXITSTATUS(status);
        } else {
            exitCode = -1;
        }
    }
    return output;
}

nlohmann::json FlashManager::listUsbDevices()
{
    std::string bin = ensureUsbMatrixBinary();
    int exitCode = 0;
    std::string output = executeCommand(bin + " -l", exitCode, 10);

    nlohmann::json devices = nlohmann::json::array();
    bool recoveryDeviceDetected = false;

    std::istringstream stream(output);
    std::string line;
    std::regex devRegex(R"(\[(\d+)\]\s+([0-9a-fA-F]+)\s+([0-9a-fA-F]+))");

    while (std::getline(stream, line)) {
        std::smatch match;
        if (std::regex_search(line, match, devRegex)) {
            std::string idx = match[1];
            std::string vid = match[2];
            std::string pid = match[3];

            // Convert to lowercase
            for (auto &c : vid) c = static_cast<char>(tolower(c));
            for (auto &c : pid) c = static_cast<char>(tolower(c));

            if (vid == "4255") {
                recoveryDeviceDetected = true;
            }

            devices.push_back({
                {"index", std::stoi(idx)},
                {"vendor", vid},
                {"product", pid}
            });
        }
    }

    return {
        {"success", exitCode == 0},
        {"recovery_device_detected", recoveryDeviceDetected},
        {"devices", devices},
        {"raw_output", output}
    };
}

FlashResult FlashManager::flashTarget(const FlashParams &params)
{
    FlashResult result;
    result.target = params.target;

    auto now = std::chrono::system_clock::now();
    std::time_t tt = std::chrono::system_clock::to_time_t(now);
    std::tm tmBuf;
    gmtime_r(&tt, &tmBuf);
    char timeStr[64];
    std::strftime(timeStr, sizeof(timeStr), "%Y-%m-%dT%H:%M:%SZ", &tmBuf);
    result.timestampIso = timeStr;

    if (_isFlashing.exchange(true)) {
        result.success = false;
        result.stage = "rejected";
        result.error = "Another firmware flashing operation is currently in progress";
        std::lock_guard<std::mutex> lock(_mutex);
        _lastResult = result;
        return result;
    }

    auto startTime = std::chrono::steady_clock::now();
    _flashStartTime = startTime;

    auto updateState = [&](const std::string &stage, bool success = false, const std::string &error = "") {
        result.stage = stage;
        result.success = success;
        if (stage == "ram_boot_completed") {
            result.mode = "ram_boot";
        }
        if (!error.empty()) {
            result.error = error;
        }
        auto nowTime = std::chrono::steady_clock::now();
        result.durationMs = static_cast<int>(
            std::chrono::duration_cast<std::chrono::milliseconds>(nowTime - startTime).count());
        std::lock_guard<std::mutex> lock(_mutex);
        _lastResult = result;
    };

    updateState("preflight");

    // 1. Pre-flight checks & resolve target defaults
    auto targetObj = TargetManager::getInstance().getTarget(params.target);
    if (!targetObj) {
        _isFlashing = false;
        updateState("preflight", false, "Target '" + params.target + "' not found in TargetManager");
        return result;
    }

    // Suppress MCU polling for the duration of the flashing session
    Target::ScopedMcuPollPause mcuPollPause(targetObj);

    // Look up target configuration defaults
    TargetFlashConfig flashCfg;
    for (const auto &tc : Config::getInstance().getTargets()) {
        if (tc.name == params.target) {
            flashCfg = tc.flashing;
            break;
        }
    }

    std::string ustImage = !params.ustImage.empty() ? Config::resolveHomePath(params.ustImage) : flashCfg.ustImage;
    std::string bldImage = !params.bldImage.empty() ? Config::resolveHomePath(params.bldImage) : flashCfg.bldImage;
    std::string chip = !params.chip.empty() ? params.chip : flashCfg.chip;

    // Detect if this is a BLD RAM boot:
    // 1) User explicitly supplied bldImage but left fwImage empty
    // 2) fwImage matches bldImage or fwImage name contains "u-boot"
    bool isRamBoot = false;
    std::string fwImage;
    if (!params.bldImage.empty() && params.fwImage.empty()) {
        isRamBoot = true;
    } else {
        fwImage = !params.fwImage.empty() ? Config::resolveHomePath(params.fwImage) : flashCfg.defaultFwImage;
        if (!bldImage.empty() && (bldImage == fwImage || fwImage.find("u-boot") != std::string::npos)) {
            isRamBoot = true;
        }
    }
    result.mode = isRamBoot ? "ram_boot" : "emmc_flash";

    if (!isRamBoot && fwImage.empty()) {
        _isFlashing = false;
        updateState("preflight", false, "No firmware package image specified or configured for target '" + params.target + "'");
        return result;
    }

    if (!fwImage.empty() && access(fwImage.c_str(), R_OK) != 0) {
        _isFlashing = false;
        updateState("preflight", false, "Firmware image file not found or unreadable: " + fwImage);
        return result;
    }

    if (!ustImage.empty() && access(ustImage.c_str(), R_OK) != 0) {
        _isFlashing = false;
        updateState("preflight", false, "UST image file not found or unreadable: " + ustImage);
        return result;
    }

    if (!bldImage.empty() && access(bldImage.c_str(), R_OK) != 0) {
        _isFlashing = false;
        updateState("preflight", false, "BLD image file not found or unreadable: " + bldImage);
        return result;
    }

    // 2. Put board into forced USB boot mode if requested
    if (params.forceUsbBoot && targetObj->getMcu()) {
        updateState("usb_recovery_boot");
        
        bool detected = false;
        // Hardware quirk: Sometimes the MCU state-latches. We allow up to 2 attempts.
        for (int attempt = 1; attempt <= 2 && !detected; ++attempt) {
            bool mcuOk = targetObj->getMcu()->resetUsbDownload(true);
            if (!mcuOk) {
                _isFlashing = false;
                updateState("usb_recovery_boot", false, "Failed to trigger MCU resetUsbDownload for target '" + params.target + "'");
                return result;
            }

            // Wait up to 5 seconds for USB device enumeration
            for (int waitCount = 0; waitCount < 10; ++waitCount) {
                std::this_thread::sleep_for(std::chrono::milliseconds(500));
                auto devList = listUsbDevices();
                if (devList.value("recovery_device_detected", false)) {
                    detected = true;
                    break;
                }
            }
        }

        if (!detected) {
            _isFlashing = false;
            updateState("usb_recovery_boot", false, "Board failed to enumerate on USB bus in ROM0 mode after MCU reset. Flashing aborted.");
            return result;
        }
    }

    // 3. Build and execute usb-matrix command
    updateState("flashing");
    std::string bin = ensureUsbMatrixBinary();

    std::string cmd = bin;
    if (!chip.empty()) {
        cmd += " -c " + chip;
    }
    if (!ustImage.empty()) {
        cmd += " -u \"" + ustImage + "\"";
    }
    if (!bldImage.empty()) {
        cmd += " -b \"" + bldImage + "\"";
    }
    if (!fwImage.empty()) {
        cmd += " -f \"" + fwImage + "\"";
    }

    int exitCode = 0;
    
    auto outputCallback = [&](const std::string &log) {
        result.usbMatrixLog = log;
        auto nowTime = std::chrono::steady_clock::now();
        result.durationMs = static_cast<int>(
            std::chrono::duration_cast<std::chrono::milliseconds>(nowTime - startTime).count());
        std::lock_guard<std::mutex> mlock(_mutex);
        _lastResult = result;
    };
    
    std::string log = executeCommand(cmd, exitCode, params.timeoutSec, outputCallback);
    result.usbMatrixLog = log;

    auto endTime = std::chrono::steady_clock::now();
    result.durationMs = static_cast<int>(
        std::chrono::duration_cast<std::chrono::milliseconds>(endTime - startTime).count());

    bool consoleBootVerified = false;
    if (exitCode != 0) {
        // Check if BLD / U-Boot successfully loaded and booted to interactive prompt on console0
        auto console0 = targetObj->getConsole(0);
        if (!console0) {
            auto consoles = targetObj->getConsoles();
            if (!consoles.empty()) console0 = consoles[0];
        }

        if (console0) {
            std::this_thread::sleep_for(std::chrono::milliseconds(500));
            auto lines = console0->getRecentLines(20);
            for (const auto &l : lines) {
                if (l.find("=> ") != std::string::npos ||
                    l.find("U-Boot 20") != std::string::npos ||
                    l.find("Hit any key to stop autoboot") != std::string::npos) {
                    consoleBootVerified = true;
                    break;
                }
            }
        }
    }

    if (exitCode == 0) {
        updateState(isRamBoot ? "ram_boot_completed" : "completed", true);
    } else if (consoleBootVerified) {
        result.error = "";
        result.mode = "ram_boot";
        updateState("ram_boot_completed", true);
    } else {
        updateState("failed", false, "usb-matrix failed with exit code " + std::to_string(exitCode));
    }

    UsageDatabase::getInstance().recordEvent(
        params.target,
        "flash",
        result.success ? "success" : "failed",
        result.durationMs,
        result.error.empty() ? result.stage : result.error);

    _isFlashing = false;
    return result;
}

FlashResult FlashManager::getLastResult(const std::string &target) const
{
    std::lock_guard<std::mutex> lock(_mutex);
    if (!target.empty() && !_lastResult.target.empty() && _lastResult.target != target) {
        FlashResult empty;
        empty.target = target;
        empty.stage = "none";
        return empty;
    }
    FlashResult res = _lastResult;
    if (res.stage == "ram_boot_completed") {
        res.mode = "ram_boot";
    }
    if (_isFlashing.load()) {
        auto nowTime = std::chrono::steady_clock::now();
        res.durationMs = static_cast<int>(
            std::chrono::duration_cast<std::chrono::milliseconds>(nowTime - _flashStartTime).count());
    }
    return res;
}

bool FlashManager::isFlashing() const
{
    return _isFlashing.load();
}

} // namespace embdevenv

/*
 * Local variables:
 * mode: C++
 * c-file-style: "BSD"
 * c-basic-offset: 4
 * tab-width: 4
 * indent-tabs-mode: nil
 * End:
 */
