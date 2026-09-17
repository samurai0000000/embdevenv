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

namespace embdevenv {

N1655Mcu::N1655Mcu(const std::string &port, int baudRate, int pollIntervalMs)
    : _port(port)
    , _baudRate(baudRate)
    , _pollIntervalMs(pollIntervalMs > 0 ? pollIntervalMs : 2000)
    , _serial(_port, _baudRate)
    , _telemetryStore(std::make_shared<TelemetryStore>(1000))
    , _running(false)
    , _connected(false)
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

std::shared_ptr<TelemetryStore> N1655Mcu::getTelemetryStore() const
{
    return _telemetryStore;
}

bool N1655Mcu::recoverPrompt(int timeoutMs)
{
    // Send Ctrl-C to cancel any active streaming or partial input
    const char ctrlC = '\003';
    _serial.write(&ctrlC, 1);
    _serial.flush();

    // Send a return
    _serial.write("\r", 1);

    char buf[128];
    auto start = std::chrono::steady_clock::now();
    std::string resp;

    while (std::chrono::duration_cast<std::chrono::milliseconds>(
               std::chrono::steady_clock::now() - start).count() < timeoutMs) {
        ssize_t n = _serial.read(buf, sizeof(buf) - 1, 100);
        if (n > 0) {
            buf[n] = '\0';
            resp += buf;
            if (resp.find("# ") != std::string::npos || resp.find('#') != std::string::npos) {
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
    }

    // Clean buffer
    _serial.flush();

    // Send command with carriage return
    std::string fullCmd = cmd;
    if (fullCmd.empty() || fullCmd.back() != '\r') {
        fullCmd += '\r';
    }
    _serial.write(fullCmd);

    // Read until "# " prompt or timeout
    std::string response;
    char buf[256];
    auto start = std::chrono::steady_clock::now();

    while (std::chrono::duration_cast<std::chrono::milliseconds>(
               std::chrono::steady_clock::now() - start).count() < timeoutMs) {
        ssize_t n = _serial.read(buf, sizeof(buf) - 1, 100);
        if (n > 0) {
            buf[n] = '\0';
            response += buf;

            // Prompt ended with "# "
            if (response.size() >= 2 && response.substr(response.size() - 2) == "# ") {
                break;
            }
        } else if (n < 0) {
            _connected = false;
            _serial.close();
            break;
        }
    }

    // Trim trailing prompt "# " if present
    if (response.size() >= 2 && response.substr(response.size() - 2) == "# ") {
        response.erase(response.size() - 2);
    }

    return response;
}

bool N1655Mcu::powerOn()
{
    std::string resp = sendCommand("power on", 3000);
    return !resp.empty();
}

bool N1655Mcu::powerOff()
{
    std::string resp = sendCommand("power off", 3000);
    return !resp.empty();
}

bool N1655Mcu::reboot()
{
    std::string resp = sendCommand("reboot", 3000);
    return !resp.empty();
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

    // Pattern for voltage: KEY=12.34V or KEY=1.8V
    std::regex vRegex("([A-Za-z0-9_]+)=([0-9]+\\.?[0-9]*)V");
    auto vBegin = std::sregex_iterator(block.begin(), block.end(), vRegex);
    auto vEnd = std::sregex_iterator();
    for (auto it = vBegin; it != vEnd; ++it) {
        std::smatch m = *it;
        std::string key = m[1].str();
        double val = std::stod(m[2].str());
        sample.voltages[key] = val;
        foundAny = true;
    }

    // Temperature pattern: TEMP=42.5C or TEMP=45C
    std::regex tRegex("(?:TEMP|temp)=([0-9]+\\.?[0-9]*)C?");
    std::smatch tMatch;
    if (std::regex_search(block, tMatch, tRegex)) {
        sample.temperatureC = std::stod(tMatch[1].str());
        foundAny = true;
    }

    // Fan pattern: FAN=2400RPM or FAN=2400
    std::regex fRegex("(?:FAN|fan)=([0-9]+)(?:RPM)?");
    std::smatch fMatch;
    if (std::regex_search(block, fMatch, fRegex)) {
        sample.fanRpm = std::stoi(fMatch[1].str());
        foundAny = true;
    }

    // Power status based on SYS_5V or any detected system voltage
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
    while (_running) {
        bool connected = false;
        {
            std::lock_guard<std::mutex> lock(_cmdMutex);
            if (!_serial.isOpen()) {
                if (_serial.open(_port, _baudRate)) {
                    _connected = true;
                    recoverPrompt(500);
                } else {
                    _connected = false;
                }
            } else {
                _connected = true;
            }
            connected = _connected;
        }

        if (connected) {
            std::string resp = sendCommand("bs", 1500);
            if (!resp.empty()) {
                TelemetrySample sample;
                if (parseTelemetryBlock(resp, sample)) {
                    _telemetryStore->addSample(sample);
                }
            }
        }

        std::unique_lock<std::mutex> lock(_cmdMutex);
        _cv.wait_for(lock, std::chrono::milliseconds(_pollIntervalMs), [this] {
            return !_running;
        });
    }
}

nlohmann::json N1655Mcu::getStatusJson() const
{
    nlohmann::json j;
    j["port"] = _port;
    j["baudRate"] = _baudRate;
    j["running"] = _running.load();
    j["connected"] = _connected.load();
    j["pollIntervalMs"] = _pollIntervalMs;

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
