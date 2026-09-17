/*
 * TargetFactory.hxx
 *
 * Copyright (C) 2026, Charles Chiou
 */

#ifndef EMBDEVENV_TARGET_FACTORY_HXX
#define EMBDEVENV_TARGET_FACTORY_HXX

#include <memory>
#include <string>
#include <vector>
#include "Target.hxx"

namespace embdevenv {

struct ConsoleConfig {
    std::string name;
    std::string port;
    int baudRate = 115200;
    int telnetPort = 0;
    size_t lineBufferCapacity = 10000;
};

struct McuConfig {
    std::string port;
    int baudRate = 115200;
    std::string prompt = "#";
    int telnetPort = 0;
    int pollIntervalMs = 2000;
};

struct TargetConfig {
    std::string name;
    std::string model;
    std::string description;
    bool enabled = true;

    // Multi-console & MCU configuration
    std::vector<ConsoleConfig> consoles;
    McuConfig mcu;

    // Backwards-compatible flat fields
    int telnetPort = 0;
    std::string consolePort;
    int consoleBaudRate = 115200;
    std::string mcuPort;
    int mcuBaudRate = 115200;
    int mcuPollIntervalMs = 2000;
};

class TargetFactory {
public:
    static std::shared_ptr<Target> createTarget(const TargetConfig &config);
};

} // namespace embdevenv

#endif // EMBDEVENV_TARGET_FACTORY_HXX

/*
 * Local variables:
 * mode: C++
 * c-file-style: "BSD"
 * c-basic-offset: 4
 * tab-width: 4
 * indent-tabs-mode: nil
 * End:
 */
