/*
 * Target.hxx
 *
 * Copyright (C) 2026, Charles Chiou
 */

#ifndef EMBDEVENV_TARGET_HXX
#define EMBDEVENV_TARGET_HXX

#include <string>
#include <memory>
#include <nlohmann/json.hpp>
#include "Console.hxx"
#include "Mcu.hxx"
#include "TelnetServer.hxx"

namespace embdevenv {

class Target {
public:
    Target(const std::string &name,
           const std::string &model,
           const std::string &description,
           bool enabled,
           int telnetPort);
    virtual ~Target();

    virtual bool start();
    virtual void stop();
    virtual bool isRunning() const;

    const std::string &getName() const;
    const std::string &getModel() const;
    const std::string &getDescription() const;
    bool isEnabled() const;
    int getTelnetPort() const;

    std::shared_ptr<Console> getConsole() const;
    std::shared_ptr<Mcu> getMcu() const;
    std::shared_ptr<TelnetServer> getTelnetServer() const;

    virtual nlohmann::json getStatusJson() const;

protected:
    std::string _name;
    std::string _model;
    std::string _description;
    bool _enabled;
    int _telnetPort;
    bool _running;

    std::shared_ptr<Console> _console;
    std::shared_ptr<Mcu> _mcu;
    std::shared_ptr<TelnetServer> _telnetServer;
};

} // namespace embdevenv

#endif // EMBDEVENV_TARGET_HXX

/*
 * Local variables:
 * mode: C++
 * c-file-style: "BSD"
 * c-basic-offset: 4
 * tab-width: 4
 * indent-tabs-mode: nil
 * End:
 */
