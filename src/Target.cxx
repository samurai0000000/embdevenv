/*
 * Target.cxx
 *
 * Copyright (C) 2026, Charles Chiou
 */

#include "Target.hxx"
#include "Config.hxx"
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

    for (auto &c : _consoles) {
        if (c != nullptr) {
            c->start();
        }
    }

    if (_mcu != nullptr) {
        _mcu->start();
    }

    for (auto &ts : _telnetServers) {
        if (ts != nullptr) {
            ts->start();
        }
    }

    fprintf(stderr, "[embdevenv] Started target '%s' (%s) with %zu console(s) and %zu telnet server(s)\n",
            _name.c_str(), _model.c_str(), _consoles.size(), _telnetServers.size());
    return true;
}

void Target::stop()
{
    if (!_running) {
        return;
    }

    _running = false;

    for (auto &ts : _telnetServers) {
        if (ts != nullptr) {
            ts->stop();
        }
    }

    if (_mcu != nullptr) {
        _mcu->stop();
    }

    for (auto &c : _consoles) {
        if (c != nullptr) {
            c->stop();
        }
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

void Target::addConsole(std::shared_ptr<Console> console)
{
    if (console != nullptr) {
        _consoles.push_back(console);
        console->setActivityHook([this](bool active) {
            if (active) {
                pauseMcuPolling();
            } else {
                resumeMcuPolling();
            }
        });
    }
}

const std::vector<std::shared_ptr<Console>> &Target::getConsoles() const
{
    return _consoles;
}

std::shared_ptr<Console> Target::getConsole(size_t index) const
{
    if (index < _consoles.size()) {
        return _consoles[index];
    }
    return nullptr;
}

std::shared_ptr<Console> Target::getConsoleByName(const std::string &name) const
{
    for (const auto &c : _consoles) {
        if (c && c->getName() == name) {
            return c;
        }
    }
    return nullptr;
}

void Target::setMcu(std::shared_ptr<Mcu> mcu)
{
    _mcu = mcu;
}

std::shared_ptr<Mcu> Target::getMcu() const
{
    return _mcu;
}

void Target::pauseMcuPolling()
{
    if (_mcu != nullptr) {
        _mcu->pausePolling();
    }
}

void Target::resumeMcuPolling()
{
    if (_mcu != nullptr) {
        _mcu->resumePolling();
    }
}

bool Target::isMcuPollingPaused() const
{
    return _mcu != nullptr && _mcu->isPollingPaused();
}

void Target::addTelnetServer(std::shared_ptr<TelnetServer> telnetServer)
{
    if (telnetServer != nullptr) {
        _telnetServers.push_back(telnetServer);
    }
}

const std::vector<std::shared_ptr<TelnetServer>> &Target::getTelnetServers() const
{
    return _telnetServers;
}

std::shared_ptr<TelnetServer> Target::getTelnetServer() const
{
    if (!_telnetServers.empty()) {
        return _telnetServers[0];
    }
    return nullptr;
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

    nlohmann::json consolesJson = nlohmann::json::array();
    for (const auto &c : _consoles) {
        if (c) {
            nlohmann::json cj;
            cj["name"] = c->getName();
            cj["port"] = c->getPort();
            cj["baudRate"] = c->getBaudRate();
            cj["telnetPort"] = c->getTelnetPort();
            cj["connected"] = c->isConnected();
            consolesJson.push_back(cj);
        }
    }
    j["consoles"] = consolesJson;

    if (!_consoles.empty() && _consoles[0] != nullptr) {
        nlohmann::json c;
        c["name"] = _consoles[0]->getName();
        c["port"] = _consoles[0]->getPort();
        c["baudRate"] = _consoles[0]->getBaudRate();
        c["telnetPort"] = _consoles[0]->getTelnetPort();
        c["connected"] = _consoles[0]->isConnected();
        j["console"] = c;
    } else {
        j["console"] = nullptr;
    }

    if (_mcu != nullptr) {
        j["mcu"] = _mcu->getStatusJson();
    } else {
        j["mcu"] = nullptr;
    }

    nlohmann::json telnetsJson = nlohmann::json::array();
    for (const auto &ts : _telnetServers) {
        if (ts) {
            nlohmann::json tj;
            tj["port"] = ts->getPort();
            tj["activeSessions"] = ts->getActiveSessionCount();
            tj["running"] = ts->isRunning();
            telnetsJson.push_back(tj);
        }
    }
    j["telnetServers"] = telnetsJson;

    if (!_telnetServers.empty() && _telnetServers[0] != nullptr) {
        nlohmann::json t;
        t["port"] = _telnetServers[0]->getPort();
        t["activeSessions"] = _telnetServers[0]->getActiveSessionCount();
        t["running"] = _telnetServers[0]->isRunning();
        j["telnet"] = t;
    } else {
        j["telnet"] = nullptr;
    }

    for (const auto &tc : Config::getInstance().getTargets()) {
        if (tc.name == _name) {
            j["flashing"] = {
                {"chip", tc.flashing.chip},
                {"ustImage", tc.flashing.ustImage},
                {"bldImage", tc.flashing.bldImage},
                {"defaultFwImage", tc.flashing.defaultFwImage}
            };
            break;
        }
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
