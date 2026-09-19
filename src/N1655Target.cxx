/*
 * N1655Target.cxx
 *
 * Copyright (C) 2026, Charles Chiou
 */

#include "N1655Target.hxx"
#include "Console.hxx"
#include "N1655Mcu.hxx"
#include "TelnetServer.hxx"

namespace embdevenv {

N1655Target::N1655Target(const TargetConfig &config)
    : Target(config.name, config.model, config.description, config.enabled, config.telnetPort)
{
    // Configure consoles
    if (!config.consoles.empty()) {
        int defaultIdx = 0;
        for (const auto &cc : config.consoles) {
            if (!cc.port.empty() || !config.usbPath.empty()) {
                auto console = std::make_shared<Console>(
                    cc.name, cc.port, cc.baudRate, cc.lineBufferCapacity, cc.telnetPort);
                if (!config.usbPath.empty()) {
                    int pIdx = (cc.portIndex >= 0) ? cc.portIndex : defaultIdx;
                    console->setUsbBinding(config.usbPath, pIdx);
                }
                addConsole(console);

                if (cc.telnetPort > 0) {
                    auto telnetServer = std::make_shared<TelnetServer>(cc.telnetPort, console.get());
                    addTelnetServer(telnetServer);
                }
            }
            defaultIdx++;
        }
    } else if (!config.consolePort.empty() || !config.usbPath.empty()) {
        auto console = std::make_shared<Console>(
            "console0", config.consolePort, config.consoleBaudRate, 10000, config.telnetPort);
        if (!config.usbPath.empty()) {
            console->setUsbBinding(config.usbPath, 0);
        }
        addConsole(console);

        if (config.telnetPort > 0) {
            auto telnetServer = std::make_shared<TelnetServer>(config.telnetPort, console.get());
            addTelnetServer(telnetServer);
        }
    }

    // Configure MCU
    if (!config.mcu.port.empty() || !config.usbPath.empty()) {
        auto mcu = std::make_shared<N1655Mcu>(
            config.mcu.port, config.mcu.baudRate, config.mcu.pollIntervalMs, config.mcu.telnetPort);
        if (!config.usbPath.empty()) {
            int pIdx = (config.mcu.portIndex >= 0) ? config.mcu.portIndex : 3;
            mcu->setUsbBinding(config.usbPath, pIdx);
        }
        setMcu(mcu);

        if (config.mcu.telnetPort > 0) {
            auto mcuTelnet = std::make_shared<TelnetServer>(config.mcu.telnetPort, mcu.get());
            addTelnetServer(mcuTelnet);
        }
    } else if (!config.mcuPort.empty()) {
        auto mcu = std::make_shared<N1655Mcu>(
            config.mcuPort, config.mcuBaudRate, config.mcuPollIntervalMs);
        setMcu(mcu);
    }
}

N1655Target::N1655Target(const std::string &name,
                         const std::string &model,
                         const std::string &description,
                         bool enabled,
                         int telnetPort,
                         const std::string &consolePort,
                         int consoleBaudRate,
                         const std::string &mcuPort,
                         int mcuBaudRate,
                         int mcuPollIntervalMs)
    : Target(name, model, description, enabled, telnetPort)
{
    // Instantiate console
    if (!consolePort.empty()) {
        auto console = std::make_shared<Console>("console0", consolePort, consoleBaudRate, 10000, telnetPort);
        addConsole(console);
        if (_telnetPort > 0) {
            auto telnetServer = std::make_shared<TelnetServer>(_telnetPort, console.get());
            addTelnetServer(telnetServer);
        }
    }

    // Instantiate MCU
    if (!mcuPort.empty()) {
        auto mcu = std::make_shared<N1655Mcu>(mcuPort, mcuBaudRate, mcuPollIntervalMs);
        setMcu(mcu);
    }
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
