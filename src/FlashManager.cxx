/*
 * FlashManager.cxx
 *
 * Copyright (C) 2026, Charles Chiou
 */

#include "FlashManager.hxx"
#include "UsbMatrixEmbedded.hxx"
#include "Config.hxx"
#include "TargetManager.hxx"
#include <sys/stat.h>
#include <sys/types.h>
#include <unistd.h>
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

std::string FlashManager::executeCommand(const std::string &cmd, int &exitCode, int timeoutSec)
{
    (void)timeoutSec;
    std::string fullCmd = cmd + " 2>&1";
    FILE *fp = popen(fullCmd.c_str(), "r");
    if (!fp) {
        exitCode = -1;
        return "Failed to run command: " + cmd;
    }

    std::string output;
    char buf[512];
    while (fgets(buf, sizeof(buf), fp) != nullptr) {
        output += buf;
    }

    int status = pclose(fp);
    if (WIFEXITED(status)) {
        exitCode = WEXITSTATUS(status);
    } else {
        exitCode = -1;
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
        return result;
    }

    auto startTime = std::chrono::steady_clock::now();

    // 1. Pre-flight checks & resolve target defaults
    auto targetObj = TargetManager::getInstance().getTarget(params.target);
    if (!targetObj) {
        _isFlashing = false;
        result.success = false;
        result.stage = "preflight";
        result.error = "Target '" + params.target + "' not found in TargetManager";
        return result;
    }

    // Look up target configuration defaults
    TargetFlashConfig flashCfg;
    for (const auto &tc : Config::getInstance().getTargets()) {
        if (tc.name == params.target) {
            flashCfg = tc.flashing;
            break;
        }
    }

    std::string fwImage = !params.fwImage.empty() ? Config::resolveHomePath(params.fwImage) : flashCfg.defaultFwImage;
    std::string ustImage = !params.ustImage.empty() ? Config::resolveHomePath(params.ustImage) : flashCfg.ustImage;
    std::string bldImage = !params.bldImage.empty() ? Config::resolveHomePath(params.bldImage) : flashCfg.bldImage;
    std::string chip = !params.chip.empty() ? params.chip : flashCfg.chip;

    if (fwImage.empty()) {
        _isFlashing = false;
        result.success = false;
        result.stage = "preflight";
        result.error = "No firmware package image specified or configured for target '" + params.target + "'";
        return result;
    }

    if (access(fwImage.c_str(), R_OK) != 0) {
        _isFlashing = false;
        result.success = false;
        result.stage = "preflight";
        result.error = "Firmware image file not found or unreadable: " + fwImage;
        return result;
    }

    if (!ustImage.empty() && access(ustImage.c_str(), R_OK) != 0) {
        _isFlashing = false;
        result.success = false;
        result.stage = "preflight";
        result.error = "UST image file not found or unreadable: " + ustImage;
        return result;
    }

    if (!bldImage.empty() && access(bldImage.c_str(), R_OK) != 0) {
        _isFlashing = false;
        result.success = false;
        result.stage = "preflight";
        result.error = "BLD image file not found or unreadable: " + bldImage;
        return result;
    }

    // 2. Put board into forced USB boot mode if requested
    if (params.forceUsbBoot && targetObj->getMcu()) {
        result.stage = "usb_recovery_boot";
        bool mcuOk = targetObj->getMcu()->resetUsbDownload(true);
        if (!mcuOk) {
            _isFlashing = false;
            result.success = false;
            result.stage = "usb_recovery_boot";
            result.error = "Failed to trigger MCU resetUsbDownload for target '" + params.target + "'";
            return result;
        }

        // Wait up to 5 seconds for USB device enumeration
        bool detected = false;
        for (int waitCount = 0; waitCount < 10; ++waitCount) {
            std::this_thread::sleep_for(std::chrono::milliseconds(500));
            auto devList = listUsbDevices();
            if (devList.value("recovery_device_detected", false)) {
                detected = true;
                break;
            }
        }
        (void)detected;
    }

    // 3. Build and execute usb-matrix command
    result.stage = "flashing";
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
    cmd += " -f \"" + fwImage + "\"";

    int exitCode = 0;
    std::string log = executeCommand(cmd, exitCode, params.timeoutSec);
    result.usbMatrixLog = log;

    auto endTime = std::chrono::steady_clock::now();
    result.durationMs = static_cast<int>(
        std::chrono::duration_cast<std::chrono::milliseconds>(endTime - startTime).count());

    if (exitCode == 0) {
        result.success = true;
        result.stage = "completed";
    } else {
        result.success = false;
        result.stage = "failed";
        result.error = "usb-matrix failed with exit code " + std::to_string(exitCode);
    }

    {
        std::lock_guard<std::mutex> lock(_mutex);
        _lastResult = result;
    }

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
    return _lastResult;
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
