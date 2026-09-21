/*
 * Target.hxx
 *
 * Copyright (C) 2026, Charles Chiou
 */

#ifndef EMBDEVENV_TARGET_HXX
#define EMBDEVENV_TARGET_HXX

#include <string>
#include <vector>
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
           int telnetPort = 0);
    virtual ~Target();

    virtual bool start();
    virtual void stop();
    virtual bool isRunning() const;

    const std::string &getName() const;
    const std::string &getModel() const;
    const std::string &getDescription() const;
    bool isEnabled() const;
    int getTelnetPort() const;

    void addConsole(std::shared_ptr<Console> console);
    const std::vector<std::shared_ptr<Console>> &getConsoles() const;
    std::shared_ptr<Console> getConsole(size_t index = 0) const;
    std::shared_ptr<Console> getConsoleByName(const std::string &name) const;

    void setMcu(std::shared_ptr<Mcu> mcu);
    std::shared_ptr<Mcu> getMcu() const;

    void pauseMcuPolling();
    void resumeMcuPolling();
    bool isMcuPollingPaused() const;

    struct ScopedMcuPollPause {
        explicit ScopedMcuPollPause(Target *target) : _target(target) {
            if (_target != nullptr) {
                _target->pauseMcuPolling();
            }
        }
        explicit ScopedMcuPollPause(const std::shared_ptr<Target> &target) : _target(target ? target.get() : nullptr) {
            if (_target != nullptr) {
                _target->pauseMcuPolling();
            }
        }
        ~ScopedMcuPollPause() {
            if (_target != nullptr) {
                _target->resumeMcuPolling();
            }
        }
        ScopedMcuPollPause(const ScopedMcuPollPause &) = delete;
        ScopedMcuPollPause &operator=(const ScopedMcuPollPause &) = delete;
    private:
        Target *_target = nullptr;
    };

    void addTelnetServer(std::shared_ptr<TelnetServer> telnetServer);
    const std::vector<std::shared_ptr<TelnetServer>> &getTelnetServers() const;
    std::shared_ptr<TelnetServer> getTelnetServer() const;

    virtual nlohmann::json getStatusJson() const;

protected:
    std::string _name;
    std::string _model;
    std::string _description;
    bool _enabled;
    int _telnetPort;
    bool _running;

    std::vector<std::shared_ptr<Console>> _consoles;
    std::shared_ptr<Mcu> _mcu;
    std::vector<std::shared_ptr<TelnetServer>> _telnetServers;
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
