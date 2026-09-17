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
    static std::string toolMcuGetPowerState(const std::string &target);
    static std::string toolMcuGetTelemetry(const std::string &target);
    static std::string toolMcuGetVoltages(const std::string &target);
    static std::string toolMcuGetTemperature(const std::string &target);
    static std::string toolMcuGetFanSpeed(const std::string &target);
    static std::string toolMcuGetTelemetryHistory(const std::string &target, size_t count);
    static std::string toolMcuSendCommand(const std::string &target, const std::string &cmd, int timeoutMs);
    static std::string toolConsoleRead(const std::string &target, size_t lines);
    static std::string toolConsoleWrite(const std::string &target, const std::string &data);
    static std::string toolConsoleClear(const std::string &target);
    static std::string toolTelnetGetStatus(const std::string &target);
    static std::string toolGetConfig();
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
