/*
 * Mcu.hxx
 *
 * Copyright (C) 2026, Charles Chiou
 */

#ifndef EMBDEVENV_MCU_HXX
#define EMBDEVENV_MCU_HXX

#include <string>
#include <memory>
#include <nlohmann/json.hpp>
#include "TelemetryStore.hxx"

namespace embdevenv {

class Mcu {
public:
    virtual ~Mcu() = default;

    enum class PowerState { UNKNOWN, OFF, ON };

    virtual bool start() = 0;
    virtual void stop() = 0;
    virtual bool isRunning() const = 0;
    virtual bool isConnected() const = 0;
    virtual PowerState getPowerState() const { return PowerState::UNKNOWN; }

    virtual bool powerOn() = 0;
    virtual bool powerOff() = 0;
    virtual bool reboot() = 0;
    virtual bool resetUsbDownload(bool doubleReset = true) = 0;
    virtual std::string sendCommand(const std::string &cmd, int timeoutMs = 2000) = 0;

    virtual std::shared_ptr<TelemetryStore> getTelemetryStore() const = 0;
    virtual nlohmann::json getStatusJson() const = 0;

    virtual const std::string &getPort() const = 0;
    virtual int getBaudRate() const = 0;

    virtual void pausePolling() {}
    virtual void resumePolling() {}
    virtual bool isPollingPaused() const { return false; }

    struct ScopedPollPause {
        explicit ScopedPollPause(Mcu *mcu) : _mcu(mcu) {
            if (_mcu != nullptr) {
                _mcu->pausePolling();
            }
        }
        explicit ScopedPollPause(const std::shared_ptr<Mcu> &mcu) : _mcu(mcu ? mcu.get() : nullptr) {
            if (_mcu != nullptr) {
                _mcu->pausePolling();
            }
        }
        ~ScopedPollPause() {
            if (_mcu != nullptr) {
                _mcu->resumePolling();
            }
        }
        ScopedPollPause(const ScopedPollPause &) = delete;
        ScopedPollPause &operator=(const ScopedPollPause &) = delete;
    private:
        Mcu *_mcu = nullptr;
    };

    const std::string &getTargetName() const { return _targetName; }
    void setTargetName(const std::string &name) { _targetName = name; }

protected:
    std::string _targetName;
};

} // namespace embdevenv

#endif // EMBDEVENV_MCU_HXX

/*
 * Local variables:
 * mode: C++
 * c-file-style: "BSD"
 * c-basic-offset: 4
 * tab-width: 4
 * indent-tabs-mode: nil
 * End:
 */
