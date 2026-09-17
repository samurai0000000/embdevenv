/*
 * Target.cxx
 *
 * Copyright (C) 2026, Charles Chiou
 */

#include "Target.hxx"
#include <cstdio>

namespace embdevenv {

Target::Target(const std::string &name,
               const std::string &model,
               const std::string &description,
               bool enabled,
               int telnetPort)
    : _name(name)
    , _model(model)
    , _description(description)
    , _enabled(enabled)
    , _telnetPort(telnetPort)
    , _running(false)
{
}

Target::~Target()
{
    stop();
}

bool Target::start()
{
    if (!_enabled) {
        fprintf(stderr, "[embdevenv] Target '%s' is disabled, skipping start\n", _name.c_str());
        return true;
    }

    if (_running) {
        return true;
    }

    _running = true;

    if (_console != nullptr) {
        _console->start();
    }

    if (_mcu != nullptr) {
        _mcu->start();
    }

    if (_telnetServer != nullptr) {
        _telnetServer->start();
    }

    fprintf(stderr, "[embdevenv] Started target '%s' (%s)\n", _name.c_str(), _model.c_str());
    return true;
}

void Target::stop()
{
    if (!_running) {
        return;
    }

    _running = false;

    if (_telnetServer != nullptr) {
        _telnetServer->stop();
    }

    if (_mcu != nullptr) {
        _mcu->stop();
    }

    if (_console != nullptr) {
        _console->stop();
    }

    fprintf(stderr, "[embdevenv] Stopped target '%s'\n", _name.c_str());
}

bool Target::isRunning() const
{
    return _running;
}

const std::string &Target::getName() const
{
    return _name;
}

const std::string &Target::getModel() const
{
    return _model;
}

const std::string &Target::getDescription() const
{
    return _description;
}

bool Target::isEnabled() const
{
    return _enabled;
}

int Target::getTelnetPort() const
{
    return _telnetPort;
}

std::shared_ptr<Console> Target::getConsole() const
{
    return _console;
}

std::shared_ptr<Mcu> Target::getMcu() const
{
    return _mcu;
}

std::shared_ptr<TelnetServer> Target::getTelnetServer() const
{
    return _telnetServer;
}

nlohmann::json Target::getStatusJson() const
{
    nlohmann::json j;
    j["name"] = _name;
    j["model"] = _model;
    j["description"] = _description;
    j["enabled"] = _enabled;
    j["running"] = _running;
    j["telnetPort"] = _telnetPort;

    if (_console != nullptr) {
        nlohmann::json c;
        c["port"] = _console->getPort();
        c["baudRate"] = _console->getBaudRate();
        c["connected"] = _console->isConnected();
        j["console"] = c;
    } else {
        j["console"] = nullptr;
    }

    if (_mcu != nullptr) {
        j["mcu"] = _mcu->getStatusJson();
    } else {
        j["mcu"] = nullptr;
    }

    if (_telnetServer != nullptr) {
        nlohmann::json t;
        t["port"] = _telnetServer->getPort();
        t["activeSessions"] = _telnetServer->getActiveSessionCount();
        t["running"] = _telnetServer->isRunning();
        j["telnet"] = t;
    } else {
        j["telnet"] = nullptr;
    }

    return j;
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
