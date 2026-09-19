/*
 * N1655Mcu.cxx
 *
 * Copyright (C) 2026, Charles Chiou
 */

#include "N1655Mcu.hxx"
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
    , _pollIntervalMs(pollIntervalMs > 0 ? pollIntervalMs : 2000)
    , _telnetPort(telnetPort)
    , _serial(_port, _baudRate)
    , _telemetryStore(std::make_shared<TelemetryStore>(1000))
    , _running(false)
    , _connected(false)
    , _activeClients(0)
    , _nextListenerId(1)
{
}

N1655Mcu::~N1655Mcu()
{
    stop();
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

    return _serial.write(data, len);
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
}

void N1655Mcu::onClientDisconnect()
{
    int cur = _activeClients.load();
    while (cur > 0 && !_activeClients.compare_exchange_weak(cur, cur - 1)) {
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
    _serial.write(&ctrlC, 1);
    _serial.write("\r", 1);
    _serial.flush();

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
        }
    }

    return false;
}

std::string N1655Mcu::sendCommand(const std::string &cmd, int timeoutMs)
{
    std::lock_guard<std::mutex> lock(_cmdMutex);

    if (!_serial.isOpen()) {
        if (!_serial.open(_port, _baudRate)) {
            _connected = false;
            return "";
        }
        _connected = true;
        _port = _serial.getDevice();
    }

    _serial.flush();

    std::string fullCmd = cmd;
    if (fullCmd.empty() || fullCmd.back() != '\r') {
        fullCmd += '\r';
    }
    _serial.write(fullCmd);

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

    return response;
}

bool N1655Mcu::powerOn()
{
    std::string resp = sendCommand("pwr on", 3000);
    return !resp.empty();
}

bool N1655Mcu::powerOff()
{
    std::string resp = sendCommand("pwr off -y", 3000);
    return !resp.empty();
}

bool N1655Mcu::reboot()
{
    std::string resp = sendCommand("reset -y", 3000);
    return !resp.empty();
}

bool N1655Mcu::resetUsbDownload(bool doubleReset)
{
    std::string resp = sendCommand("reset -usb -y", 3000);
    if (resp.empty()) {
        return false;
    }
    if (doubleReset) {
        std::this_thread::sleep_for(std::chrono::milliseconds(500));
        resp = sendCommand("reset -usb -y", 3000);
        return !resp.empty();
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

    while (_running) {
        bool connected = false;
        {
            std::lock_guard<std::mutex> lock(_cmdMutex);
            if (!_serial.isOpen()) {
                if (_serial.open(_port, _baudRate)) {
                    _connected = true;
                    _port = _serial.getDevice();
                    recoverPrompt(500);
                } else {
                    _connected = false;
                }
            } else {
                _connected = true;
            }
            connected = _connected;
        }

        if (!connected) {
            std::unique_lock<std::mutex> lock(_cmdMutex);
            _cv.wait_for(lock, std::chrono::milliseconds(1000), [this] {
                return !_running;
            });
            continue;
        }

        // If Telnet client is connected, handle interactive streaming
        if (_activeClients.load() > 0) {
            ssize_t n = 0;
            {
                std::lock_guard<std::mutex> lock(_cmdMutex);
                if (_serial.isOpen()) {
                    n = _serial.read(readBuf, sizeof(readBuf) - 1, 100);
                }
            }
            if (n > 0) {
                readBuf[n] = '\0';
                broadcastOutput(readBuf, static_cast<size_t>(n));
            }
            continue;
        }

        // Sampling telemetry via bs
        {
            std::lock_guard<std::mutex> lock(_cmdMutex);
            if (_serial.isOpen()) {
                _serial.flush();
                _serial.write("bs\r");

                std::string block;
                auto start = std::chrono::steady_clock::now();
                while (std::chrono::duration_cast<std::chrono::milliseconds>(
                           std::chrono::steady_clock::now() - start).count() < 1200) {
                    ssize_t n = _serial.read(readBuf, sizeof(readBuf) - 1, 100);
                    if (n > 0) {
                        readBuf[n] = '\0';
                        block += readBuf;
                        if (block.find("RPM") != std::string::npos) {
                            break;
                        }
                    }
                }

                // Restore prompt with Ctrl-C
                const char ctrlC = '\003';
                _serial.write(&ctrlC, 1);
                _serial.write("\r", 1);
                _serial.flush();

                // Drain trailing bytes
                auto drainStart = std::chrono::steady_clock::now();
                while (std::chrono::duration_cast<std::chrono::milliseconds>(
                           std::chrono::steady_clock::now() - drainStart).count() < 200) {
                    ssize_t n = _serial.read(readBuf, sizeof(readBuf) - 1, 50);
                    if (n <= 0) break;
                }

                if (!block.empty()) {
                    TelemetrySample sample;
                    if (parseTelemetryBlock(block, sample)) {
                        _telemetryStore->addSample(sample);
                    }
                }
            }
        }

        std::unique_lock<std::mutex> lock(_cmdMutex);
        _cv.wait_for(lock, std::chrono::milliseconds(_pollIntervalMs), [this] {
            return !_running;
        });
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

    TelemetrySample latest;
    if (_telemetryStore->getLatest(latest)) {
        j["powerOn"] = latest.powerOn;
        j["temperatureC"] = latest.temperatureC;
        j["fanRpm"] = latest.fanRpm;
        j["latestSample"] = latest.toJson();
    } else {
        j["powerOn"] = false;
        j["latestSample"] = nullptr;
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
