/*
 * N1655Mcu.cxx
 *
 * Copyright (C) 2026, Charles Chiou
 */

#include "N1655Mcu.hxx"
#include "UsageDatabase.hxx"
#include <chrono>
#include <sstream>
#include <regex>
#include <cstdio>
#include <ctime>
#include <algorithm>

namespace embdevenv {

N1655Mcu::N1655Mcu(const std::string &port, int baudRate, int pollIntervalMs, int telnetPort)
    : _port(port)
    , _baudRate(baudRate)
    , _pollIntervalMs(pollIntervalMs > 0 ? pollIntervalMs : 15000)
    , _telnetPort(telnetPort)
    , _serial(_port, _baudRate)
    , _telemetryStore(std::make_shared<TelemetryStore>(1000))
    , _running(false)
    , _connected(false)
    , _activeClients(0)
    , _pollPauseCount(0)
    , _nextListenerId(1)
{
}

N1655Mcu::~N1655Mcu()
{
    stop();
}

void N1655Mcu::pausePolling()
{
    _pollPauseCount.fetch_add(1);
    _cv.notify_all();
}

void N1655Mcu::resumePolling()
{
    int prev = _pollPauseCount.fetch_sub(1);
    if (prev <= 1) {
        _pollPauseCount.store(0);
        _cv.notify_all();
    }
}

bool N1655Mcu::isPollingPaused() const
{
    return _pollPauseCount.load() > 0;
}

bool N1655Mcu::start()
{
    if (_running) {
        return true;
    }

    _running = true;
    _workerThread = std::thread(&N1655Mcu::pollWorker, this);
    return true;
}

void N1655Mcu::stop()
{
    if (!_running) {
        return;
    }

    _running = false;
    _cv.notify_all();

    if (_workerThread.joinable()) {
        _workerThread.join();
    }

    {
        std::lock_guard<std::mutex> lock(_cmdMutex);
        _serial.close();
        _connected = false;
    }
}

bool N1655Mcu::isRunning() const
{
    return _running;
}

bool N1655Mcu::isConnected() const
{
    return _connected;
}

const std::string &N1655Mcu::getPort() const
{
    if (!_serial.getDevice().empty()) {
        return _serial.getDevice();
    }
    return _port;
}

int N1655Mcu::getBaudRate() const
{
    return _baudRate;
}

int N1655Mcu::getPollIntervalMs() const
{
    return _pollIntervalMs;
}

int N1655Mcu::getTelnetPort() const
{
    return _telnetPort;
}

std::shared_ptr<TelemetryStore> N1655Mcu::getTelemetryStore() const
{
    return _telemetryStore;
}

ssize_t N1655Mcu::write(const char *data, size_t len)
{
    if (data == nullptr || len == 0) {
        return 0;
    }

    std::lock_guard<std::mutex> lock(_cmdMutex);
    if (!_serial.isOpen()) {
        if (!_serial.open(_port, _baudRate)) {
            _connected = false;
            return -1;
        }
        _connected = true;
    }

    ssize_t written = _serial.write(data, len);
    _cv.notify_all();
    return written;
}

ssize_t N1655Mcu::write(const std::string &data)
{
    return write(data.data(), data.size());
}

int N1655Mcu::addListener(OutputCallback callback)
{
    std::lock_guard<std::mutex> lock(_listenersMutex);
    int id = _nextListenerId++;
    _listeners.emplace_back(id, std::move(callback));
    return id;
}

void N1655Mcu::removeListener(int listenerId)
{
    std::lock_guard<std::mutex> lock(_listenersMutex);
    _listeners.erase(
        std::remove_if(_listeners.begin(), _listeners.end(),
                       [listenerId](const std::pair<int, OutputCallback> &item) {
                           return item.first == listenerId;
                       }),
        _listeners.end());
}

void N1655Mcu::onClientConnect()
{
    _activeClients.fetch_add(1);
    _cv.notify_all();

    std::lock_guard<std::mutex> lock(_cmdMutex);
    _streamingActive = false;
    if (_serial.isOpen()) {
        recoverPrompt(500);
    }
}

void N1655Mcu::onClientDisconnect()
{
    int cur = _activeClients.load();
    while (cur > 0 && !_activeClients.compare_exchange_weak(cur, cur - 1)) {
    }
    _cv.notify_all();

    if (_activeClients.load() == 0) {
        std::this_thread::sleep_for(std::chrono::milliseconds(500));
        std::lock_guard<std::mutex> lock(_cmdMutex);
        if (_serial.isOpen() && !_cmdInProgress.load() && !isPollingPaused()) {
            ssize_t wb = _serial.write("bs\r");
            (void)wb;
            _streamingActive = true;
        }
    }
}

void N1655Mcu::broadcastOutput(const char *data, size_t len)
{
    std::vector<OutputCallback> callbacks;
    {
        std::lock_guard<std::mutex> lock(_listenersMutex);
        callbacks.reserve(_listeners.size());
        for (const auto &pair : _listeners) {
            callbacks.push_back(pair.second);
        }
    }

    for (const auto &cb : callbacks) {
        if (cb) {
            cb(data, len);
        }
    }
}

bool N1655Mcu::recoverPrompt(int timeoutMs)
{
    const char ctrlC = '\003';
    if (_serial.write(&ctrlC, 1) < 0 || _serial.write("\r", 1) < 0) {
        _connected = false;
        _serial.close();
        return false;
    }

    char buf[128];
    auto start = std::chrono::steady_clock::now();
    std::string resp;

    while (std::chrono::duration_cast<std::chrono::milliseconds>(
               std::chrono::steady_clock::now() - start).count() < timeoutMs) {
        ssize_t n = _serial.read(buf, sizeof(buf) - 1, 100);
        if (n > 0) {
            buf[n] = '\0';
            resp += buf;
            if (resp.find('#') != std::string::npos) {
                return true;
            }
        } else if (n < 0) {
            _connected = false;
            _serial.close();
            return false;
        }
    }

    return false;
}

std::string N1655Mcu::sendCommand(const std::string &cmd, int timeoutMs)
{
    _cmdInProgress = true;
    _cv.notify_all();

    std::lock_guard<std::mutex> lock(_cmdMutex);

    struct CmdGuard {
        std::atomic<bool> &flag;
        ~CmdGuard() { flag.store(false); }
    } guard{_cmdInProgress};

    if (!_serial.isOpen()) {
        if (!_serial.open(_port, _baudRate)) {
            _connected = false;
            return "";
        }
        _connected = true;
        _port = _serial.getDevice();
    }

    recoverPrompt(500);

    std::string fullCmd = cmd;
    if (fullCmd.empty() || fullCmd.back() != '\r') {
        fullCmd += '\r';
    }
    if (_serial.write(fullCmd) < 0) {
        _connected = false;
        _serial.close();
        return "";
    }

    std::string response;
    char buf[256];
    auto start = std::chrono::steady_clock::now();

    while (std::chrono::duration_cast<std::chrono::milliseconds>(
               std::chrono::steady_clock::now() - start).count() < timeoutMs) {
        ssize_t n = _serial.read(buf, sizeof(buf) - 1, 100);
        if (n > 0) {
            buf[n] = '\0';
            response += buf;
            broadcastOutput(buf, static_cast<size_t>(n));

            if (!response.empty() && response.back() == '#') {
                break;
            }
            if (response.size() >= 2 && response.substr(response.size() - 2) == "# ") {
                break;
            }
        } else if (n < 0) {
            _connected = false;
            _serial.close();
            break;
        }
    }

    // Trim trailing prompt
    while (!response.empty() && (response.back() == '#' || response.back() == ' ')) {
        response.pop_back();
    }

    // Re-issue bs\r if not in interactive Telnet session and polling not paused
    if (_serial.isOpen() && _activeClients.load() == 0 && !isPollingPaused()) {
        ssize_t wb = _serial.write("bs\r");
        (void)wb;
        _streamingActive = true;
    }

    return response;
}

bool N1655Mcu::powerOn()
{
    std::string resp = sendCommand("pwr on", 3000);
    bool ok = !resp.empty();
    if (!_targetName.empty()) {
        UsageDatabase::getInstance().recordEvent(_targetName, "power_on", ok ? "success" : "failed");
    }
    return ok;
}

bool N1655Mcu::powerOff()
{
    std::string resp = sendCommand("pwr off -y", 3000);
    bool ok = !resp.empty();
    if (!_targetName.empty()) {
        UsageDatabase::getInstance().recordEvent(_targetName, "power_off", ok ? "success" : "failed");
    }
    return ok;
}

bool N1655Mcu::reboot()
{
    std::string resp = sendCommand("reset -y", 3000);
    bool ok = !resp.empty();
    if (!_targetName.empty()) {
        UsageDatabase::getInstance().recordEvent(_targetName, "reboot", ok ? "success" : "failed");
    }
    return ok;
}

Mcu::PowerState N1655Mcu::getPowerState() const
{
    TelemetrySample sample;
    if (_telemetryStore && _telemetryStore->getLatest(sample)) {
        return sample.powerOn ? PowerState::ON : PowerState::OFF;
    }
    return PowerState::UNKNOWN;
}

bool N1655Mcu::resetUsbDownload(bool doubleReset)
{
    PowerState ps = getPowerState();
    if (ps == PowerState::UNKNOWN) {
        fprintf(stderr, "[embdevenv] N1655Mcu: cannot perform USB boot; target power state is unknown (no telemetry)\n");
        if (!_targetName.empty()) {
            UsageDatabase::getInstance().recordEvent(_targetName, "usb_download", "failed_unknown_power");
        }
        return false;
    }

    if (ps == PowerState::OFF) {
        // Target is powered off: power on directly into USB bootloader mode
        std::string resp = sendCommand("pwr on -usb", 3000);
        bool ok = !resp.empty();
        if (!_targetName.empty()) {
            UsageDatabase::getInstance().recordEvent(_targetName, "usb_download", ok ? "success" : "failed");
        }
        return ok;
    }

    // PowerState::ON: reboot into USB download mode
    std::string resp = sendCommand("reset -usb -y", 3000);
    if (resp.empty()) {
        if (!_targetName.empty()) {
            UsageDatabase::getInstance().recordEvent(_targetName, "usb_download", "failed");
        }
        return false;
    }
    if (doubleReset) {
        std::this_thread::sleep_for(std::chrono::milliseconds(500));
        resp = sendCommand("reset -usb -y", 3000);
        bool ok = !resp.empty();
        if (!_targetName.empty()) {
            UsageDatabase::getInstance().recordEvent(_targetName, "usb_download", ok ? "success" : "failed");
        }
        return ok;
    }
    if (!_targetName.empty()) {
        UsageDatabase::getInstance().recordEvent(_targetName, "usb_download", "success");
    }
    return true;
}

bool N1655Mcu::parseTelemetryBlock(const std::string &block, TelemetrySample &sample)
{
    auto now = std::chrono::system_clock::now();
    sample.timestampMs = std::chrono::duration_cast<std::chrono::milliseconds>(
        now.time_since_epoch()).count();

    std::time_t tt = std::chrono::system_clock::to_time_t(now);
    std::tm tmBuf;
    gmtime_r(&tt, &tmBuf);
    char timeStr[64];
    std::strftime(timeStr, sizeof(timeStr), "%Y-%m-%dT%H:%M:%SZ", &tmBuf);
    sample.timestampIso = timeStr;
    sample.raw = block;

    bool foundAny = false;

    // Pattern for voltage: KEY = 12.34V or KEY=1.8V
    std::regex vRegex("([A-Za-z0-9_]+)\\s*=\\s*([0-9]+\\.?[0-9]*)V");
    auto vBegin = std::sregex_iterator(block.begin(), block.end(), vRegex);
    auto vEnd = std::sregex_iterator();
    for (auto it = vBegin; it != vEnd; ++it) {
        std::smatch m = *it;
        std::string key = m[1].str();
        double val = std::stod(m[2].str());
        sample.voltages[key] = val;
        foundAny = true;
    }

    // Temperature pattern: Tb = 45.6C or TEMP=42.5C
    std::regex tRegex("(?:Tb|TEMP|temp)\\s*=\\s*([0-9]+\\.?[0-9]*)C?");
    std::smatch tMatch;
    if (std::regex_search(block, tMatch, tRegex)) {
        sample.temperatureC = std::stod(tMatch[1].str());
        foundAny = true;
    }

    // Fan pattern: FAN_SPEED = 1164RPM or FAN=2400
    std::regex fRegex("(?:FAN_SPEED|FAN|fan)\\s*=\\s*([0-9]+)(?:RPM)?");
    std::smatch fMatch;
    if (std::regex_search(block, fMatch, fRegex)) {
        sample.fanRpm = std::stoi(fMatch[1].str());
        foundAny = true;
    }

    // Power status based on SYS_5V or DCOUT
    auto it5v = sample.voltages.find("SYS_5V");
    if (it5v != sample.voltages.end()) {
        sample.powerOn = (it5v->second >= 4.5);
    } else {
        auto itDcout = sample.voltages.find("DCOUT");
        if (itDcout != sample.voltages.end()) {
            sample.powerOn = (itDcout->second >= 10.0);
        } else {
            sample.powerOn = false;
        }
    }

    return foundAny;
}

void N1655Mcu::pollWorker()
{
    char readBuf[256];
    std::string streamBuffer;

    struct TelemetryAccumulator {
        int count = 0;
        int64_t windowStartTime = 0;
        bool lastPowerState = false;
        bool hasLastPowerState = false;
        double tempSum = 0.0;
        double maxTemp = 0.0;
        int fanRpmSum = 0;
        std::map<std::string, double> voltageSums;
        std::map<std::string, double> minVoltages;
        bool faultDetected = false;

        void reset(int64_t nowMs, bool powerOn) {
            count = 0;
            windowStartTime = nowMs;
            lastPowerState = powerOn;
            hasLastPowerState = true;
            tempSum = 0.0;
            maxTemp = 0.0;
            fanRpmSum = 0;
            voltageSums.clear();
            minVoltages.clear();
            faultDetected = false;
        }

        void addSample(const TelemetrySample &sample, int64_t nowMs) {
            if (count == 0) {
                windowStartTime = nowMs;
                lastPowerState = sample.powerOn;
                hasLastPowerState = true;
                maxTemp = sample.temperatureC;
            } else {
                maxTemp = std::max(maxTemp, sample.temperatureC);
            }

            count++;
            tempSum += sample.temperatureC;
            fanRpmSum += sample.fanRpm;

            for (const auto &kv : sample.voltages) {
                voltageSums[kv.first] += kv.second;
                auto itMin = minVoltages.find(kv.first);
                if (itMin == minVoltages.end()) {
                    minVoltages[kv.first] = kv.second;
                } else {
                    itMin->second = std::min(itMin->second, kv.second);
                }
            }

            auto it5v = sample.voltages.find("SYS_5V");
            if (it5v != sample.voltages.end() && it5v->second < 4.5 && sample.powerOn) {
                faultDetected = true;
            }
        }

        void flushToDb(const std::string &targetName) {
            if (count == 0 || targetName.empty()) {
                return;
            }

            double avgTemp = tempSum / count;
            int avgFan = fanRpmSum / count;
            std::map<std::string, double> avgVoltages;
            for (const auto &kv : voltageSums) {
                avgVoltages[kv.first] = kv.second / count;
            }

            UsageDatabase::getInstance().recordTelemetrySample(
                targetName,
                lastPowerState,
                avgTemp,
                avgFan,
                avgVoltages,
                maxTemp,
                minVoltages,
                faultDetected
            );
        }
    } acc;

    while (_running) {
        bool connected = false;
        {
            std::lock_guard<std::mutex> lock(_cmdMutex);
            if (!_serial.isOpen()) {
                if (_serial.open(_port, _baudRate)) {
                    _connected = true;
                    _port = _serial.getDevice();
                    _consecutiveErrors = 0;
                    _streamingActive = false;
                    fprintf(stderr, "[embdevenv] N1655Mcu '%s' self-healed on %s\n",
                            _targetName.c_str(), _port.c_str());
                } else {
                    _connected = false;
                }
            } else {
                _connected = true;
            }
            connected = _connected;
        }

        if (!connected) {
            if (acc.count > 0) {
                acc.flushToDb(_targetName);
                acc.count = 0;
            }
            std::unique_lock<std::mutex> lock(_cmdMutex);
            _cv.wait_for(lock, std::chrono::milliseconds(2000), [this] {
                return !_running;
            });
            continue;
        }

        // Yield if out-of-band command is running
        if (_cmdInProgress.load()) {
            std::unique_lock<std::mutex> lock(_cmdMutex);
            _cv.wait_for(lock, std::chrono::milliseconds(50), [this] {
                return !_running || !_cmdInProgress.load();
            });
            continue;
        }

        // If polling paused (e.g. flashing), stop streaming and wait
        if (isPollingPaused()) {
            if (_streamingActive.load()) {
                std::lock_guard<std::mutex> lock(_cmdMutex);
                if (_serial.isOpen()) {
                    const char ctrlC = '\003';
                    ssize_t w0 = _serial.write(&ctrlC, 1);
                    (void)w0;
                    ssize_t w1 = _serial.write("\r", 1);
                    (void)w1;
                }
                _streamingActive = false;
            }
            if (acc.count > 0) {
                acc.flushToDb(_targetName);
                acc.count = 0;
            }
            std::unique_lock<std::mutex> lock(_cmdMutex);
            _cv.wait_for(lock, std::chrono::milliseconds(200), [this] {
                return !_running || !isPollingPaused();
            });
            continue;
        }

        // If Telnet client is connected, handle interactive console pass-through
        if (_activeClients.load() > 0) {
            if (acc.count > 0) {
                acc.flushToDb(_targetName);
                acc.count = 0;
            }
            if (_cmdInProgress.load()) {
                std::unique_lock<std::mutex> lock(_cmdMutex);
                _cv.wait_for(lock, std::chrono::milliseconds(50), [this] {
                    return !_running || !_cmdInProgress.load();
                });
                continue;
            }
            ssize_t n = 0;
            if (_serial.isOpen()) {
                n = _serial.read(readBuf, sizeof(readBuf) - 1, 100);
            }
            if (n > 0) {
                _consecutiveErrors = 0;
                readBuf[n] = '\0';
                broadcastOutput(readBuf, static_cast<size_t>(n));
            } else if (n == 0) {
                // Silent: leave FD open
            } else if (n < 0) {
                if (++_consecutiveErrors >= 3) {
                    std::lock_guard<std::mutex> lock(_cmdMutex);
                    _connected = false;
                    _streamingActive = false;
                    _serial.close();
                    _consecutiveErrors = 0;
                    std::this_thread::sleep_for(std::chrono::milliseconds(2000));
                } else {
                    _serial.flush();
                }
            }
            continue;
        }

        // Passive streaming telemetry via bs\r
        if (!_streamingActive.load()) {
            std::lock_guard<std::mutex> lock(_cmdMutex);
            if (_serial.isOpen() && !_cmdInProgress.load()) {
                ssize_t wb = _serial.write("bs\r");
                (void)wb;
                _streamingActive = true;
                streamBuffer.clear();
            }
        }

        if (_cmdInProgress.load()) {
            continue;
        }

        ssize_t n = 0;
        if (_serial.isOpen()) {
            n = _serial.read(readBuf, sizeof(readBuf) - 1, 100);
        }

        if (n > 0) {
            _consecutiveErrors = 0;
            readBuf[n] = '\0';
            streamBuffer += readBuf;

            size_t delimPos = streamBuffer.find("RPM");
            if (delimPos == std::string::npos && streamBuffer.size() > 4) {
                size_t esc2 = streamBuffer.find("\x1b[2J", 4);
                if (esc2 != std::string::npos) {
                    delimPos = esc2;
                }
            }

            if (delimPos != std::string::npos) {
                size_t endLine = streamBuffer.find('\n', delimPos);
                std::string block;
                if (endLine != std::string::npos) {
                    block = streamBuffer.substr(0, endLine + 1);
                    streamBuffer.erase(0, endLine + 1);
                } else {
                    block = streamBuffer;
                    streamBuffer.clear();
                }

                TelemetrySample sample;
                if (parseTelemetryBlock(block, sample)) {
                    _telemetryStore->addSample(sample);

                    int64_t nowMs = std::chrono::duration_cast<std::chrono::milliseconds>(
                        std::chrono::system_clock::now().time_since_epoch()).count();

                    bool powerTransition = (acc.hasLastPowerState && sample.powerOn != acc.lastPowerState);
                    bool brownout = false;
                    auto it5v = sample.voltages.find("SYS_5V");
                    if (it5v != sample.voltages.end() && it5v->second < 4.5 && acc.hasLastPowerState && acc.lastPowerState) {
                        brownout = true;
                    }

                    if (powerTransition && acc.count > 0) {
                        acc.flushToDb(_targetName);
                        acc.reset(nowMs, sample.powerOn);
                    }

                    acc.addSample(sample, nowMs);

                    if (brownout || (nowMs - acc.windowStartTime >= 5000)) {
                        acc.flushToDb(_targetName);
                        acc.reset(nowMs, sample.powerOn);
                    }
                }
            } else if (streamBuffer.size() > 4096) {
                streamBuffer.clear();
            }
        } else if (n == 0) {
            int64_t nowMs = std::chrono::duration_cast<std::chrono::milliseconds>(
                std::chrono::system_clock::now().time_since_epoch()).count();
            if (acc.count > 0 && (nowMs - acc.windowStartTime >= 5000)) {
                acc.flushToDb(_targetName);
                acc.reset(nowMs, acc.lastPowerState);
            }
        } else if (n < 0) {
            std::lock_guard<std::mutex> lock(_cmdMutex);
            if (++_consecutiveErrors >= 3) {
                _connected = false;
                _streamingActive = false;
                _serial.close();
                _consecutiveErrors = 0;
                if (acc.count > 0) {
                    acc.flushToDb(_targetName);
                    acc.count = 0;
                }
                std::this_thread::sleep_for(std::chrono::milliseconds(2000));
            } else {
                _serial.flush();
            }
        }
    }

    if (acc.count > 0) {
        acc.flushToDb(_targetName);
    }
}

void N1655Mcu::setUsbBinding(const std::string &usbPath, int portIndex)
{
    _serial.setUsbBinding(usbPath, portIndex);
}

nlohmann::json N1655Mcu::getStatusJson() const
{
    nlohmann::json j;
    j["port"] = getPort();
    j["baudRate"] = _baudRate;
    j["running"] = _running.load();
    j["connected"] = _connected.load();
    j["pollIntervalMs"] = _pollIntervalMs;
    j["telnetPort"] = _telnetPort;

    PowerState ps = getPowerState();
    std::string psStr = "unknown";
    if (ps == PowerState::ON) {
        psStr = "on";
    } else if (ps == PowerState::OFF) {
        psStr = "off";
    }
    j["powerState"] = psStr;
    j["powerOn"] = (ps == PowerState::ON);

    TelemetrySample latest;
    bool hasSample = _telemetryStore->getLatest(latest);
    if (_connected.load() && hasSample) {
        j["temperatureC"] = latest.temperatureC;
        j["fanRpm"] = latest.fanRpm;
        j["latestSample"] = latest.toJson();
    } else {
        if (hasSample) {
            j["latestSample"] = latest.toJson();
        } else {
            j["latestSample"] = nullptr;
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
