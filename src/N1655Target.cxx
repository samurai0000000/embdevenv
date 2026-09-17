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
        _console = std::make_shared<Console>(consolePort, consoleBaudRate);
    }

    // Instantiate MCU
    if (!mcuPort.empty()) {
        _mcu = std::make_shared<N1655Mcu>(mcuPort, mcuBaudRate, mcuPollIntervalMs);
    }

    // Instantiate TelnetServer bound to this target's console
    if (_telnetPort > 0 && _console != nullptr) {
        _telnetServer = std::make_shared<TelnetServer>(_telnetPort, _console.get());
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
