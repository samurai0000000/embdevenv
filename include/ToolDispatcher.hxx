/*
 * ToolDispatcher.hxx
 *
 * Copyright (C) 2026, Charles Chiou
 */

#ifndef EMBDEVENV_TOOL_DISPATCHER_HXX
#define EMBDEVENV_TOOL_DISPATCHER_HXX

#include <string>
#include <nlohmann/json.hpp>

namespace embdevenv {

class ToolDispatcher {
public:
    static nlohmann::json getToolDefinitions();
    static std::string dispatchTool(const std::string &name, const nlohmann::json &args);

private:
    static std::string toolListTargets();
    static std::string toolGetTargetStatus(const std::string &target);
    static std::string toolMcuPowerOn(const std::string &target);
    static std::string toolMcuPowerOff(const std::string &target);
    static std::string toolMcuReboot(const std::string &target);
    static std::string toolMcuUsbDownload(const std::string &target, bool doubleReset);
    static std::string toolMcuGetPowerState(const std::string &target);
    static std::string toolMcuGetTelemetry(const std::string &target);
    static std::string toolMcuGetVoltages(const std::string &target);
    static std::string toolMcuGetTemperature(const std::string &target);
    static std::string toolMcuGetFanSpeed(const std::string &target);
    static std::string toolMcuGetTelemetryHistory(const std::string &target, size_t count);
    static std::string toolMcuSendCommand(const std::string &target, const std::string &cmd, int timeoutMs);
    static std::string toolConsoleRead(const std::string &target, size_t lines, const std::string &consoleName = "");
    static std::string toolConsoleWrite(const std::string &target, const std::string &data, int charDelayMs = 0, const std::string &consoleName = "");
    static std::string toolConsoleClear(const std::string &target, const std::string &consoleName = "");
    static std::string toolConsoleCatchBootloader(const std::string &target,
                                                  const std::string &prompt,
                                                  const std::string &interruptKey,
                                                  int timeoutSec,
                                                  bool rebootFirst,
                                                  const std::string &consoleName = "");
    static std::string toolConsoleExpect(const std::string &target,
                                         const std::string &command,
                                         const std::vector<std::string> &patterns,
                                         int timeoutSec,
                                         int charDelayMs,
                                         const std::string &logFilePath,
                                         const std::string &consoleName = "");
    static std::string toolConsoleStartLog(const std::string &target,
                                           const std::string &filePath,
                                           const std::string &consoleName = "");
    static std::string toolConsoleStopLog(const std::string &target,
                                          const std::string &consoleName = "");
    static std::string toolGetBoardStats(const std::string &target, int windowDays = 30);
    static std::string toolTelnetGetStatus(const std::string &target);
    static std::string toolGetConfig();
    static std::string toolFlashTarget(const std::string &target,
                                       const std::string &chip,
                                       const std::string &ustImage,
                                       const std::string &bldImage,
                                       const std::string &fwImage,
                                       bool forceUsbBoot,
                                       int timeoutSec);
    static std::string toolUsbListDevices();
    static std::string toolFlashGetStatus(const std::string &target);
};

} // namespace embdevenv

#endif // EMBDEVENV_TOOL_DISPATCHER_HXX

/*
 * Local variables:
 * mode: C++
 * c-file-style: "BSD"
 * c-basic-offset: 4
 * tab-width: 4
 * indent-tabs-mode: nil
 * End:
 */
