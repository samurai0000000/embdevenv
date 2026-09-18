/*
 * FlashManager.hxx
 *
 * Copyright (C) 2026, Charles Chiou
 */

#ifndef EMBDEVENV_FLASH_MANAGER_HXX
#define EMBDEVENV_FLASH_MANAGER_HXX

#include <string>
#include <vector>
#include <mutex>
#include <atomic>
#include <chrono>
#include <functional>
#include <nlohmann/json.hpp>

namespace embdevenv {

struct FlashParams {
    std::string target;
    std::string fwImage;
    std::string ustImage;
    std::string bldImage;
    std::string chip;
    bool forceUsbBoot = true;
    int timeoutSec = 60;
};

struct FlashResult {
    bool success = false;
    std::string target;
    std::string stage;
    int durationMs = 0;
    std::string usbMatrixLog;
    std::string error;
    std::string timestampIso;

    nlohmann::json toJson() const;
};

class FlashManager {
public:
    static FlashManager &getInstance();

    std::string ensureUsbMatrixBinary();

    nlohmann::json listUsbDevices();

    FlashResult flashTarget(const FlashParams &params);

    FlashResult getLastResult(const std::string &target = "") const;
    bool isFlashing() const;

private:
    FlashManager();
    ~FlashManager() = default;
    FlashManager(const FlashManager &) = delete;
    FlashManager &operator=(const FlashManager &) = delete;

    std::string executeCommand(const std::string &cmd, int &exitCode, int timeoutSec = 60, std::function<void(const std::string&)> onOutput = nullptr);

    mutable std::mutex _mutex;
    std::atomic<bool> _isFlashing;
    FlashResult _lastResult;
    std::string _cachedBinaryPath;
};

} // namespace embdevenv

#endif // EMBDEVENV_FLASH_MANAGER_HXX

/*
 * Local variables:
 * mode: C++
 * c-file-style: "BSD"
 * c-basic-offset: 4
 * tab-width: 4
 * indent-tabs-mode: nil
 * End:
 */
