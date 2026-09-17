/*
 * Console.cxx
 *
 * Copyright (C) 2026, Charles Chiou
 */

#include "Console.hxx"
#include <chrono>
#include <cstdio>
#include <algorithm>

namespace embdevenv {

Console::Console(const std::string &name, const std::string &port, int baudRate,
                 size_t lineBufferCapacity, int telnetPort)
    : _name(name)
    , _port(port)
    , _baudRate(baudRate)
    , _telnetPort(telnetPort)
    , _serial(_port, _baudRate)
    , _running(false)
    , _connected(false)
    , _lineBuffer(lineBufferCapacity)
    , _lastCharWasCr(false)
    , _nextListenerId(1)
{
}

Console::Console(const std::string &port, int baudRate, size_t lineBufferCapacity)
    : _name("console")
    , _port(port)
    , _baudRate(baudRate)
    , _telnetPort(0)
    , _serial(_port, _baudRate)
    , _running(false)
    , _connected(false)
    , _lineBuffer(lineBufferCapacity)
    , _lastCharWasCr(false)
    , _nextListenerId(1)
{
}

Console::~Console()
{
    stop();
}

bool Console::start()
{
    if (_running) {
        return true;
    }

    _running = true;
    _workerThread = std::thread(&Console::readWorker, this);
    return true;
}

void Console::stop()
{
    if (!_running) {
        return;
    }

    _running = false;
    if (_workerThread.joinable()) {
        _workerThread.join();
    }

    _serial.close();
    _connected = false;
}

bool Console::isRunning() const
{
    return _running;
}

bool Console::isConnected() const
{
    return _connected;
}

ssize_t Console::write(const char *data, size_t len)
{
    if (!_serial.isOpen()) {
        return -1;
    }
    return _serial.write(data, len);
}

ssize_t Console::write(const std::string &data)
{
    return write(data.data(), data.size());
}

std::vector<std::string> Console::getRecentLines(size_t count) const
{
    std::lock_guard<std::mutex> lock(_lineMutex);
    std::vector<std::string> lines = _lineBuffer.getLatest(count);
    if (!_partialLine.empty()) {
        lines.push_back(_partialLine);
    }
    return lines;
}

void Console::clearBuffer()
{
    std::lock_guard<std::mutex> lock(_lineMutex);
    _lineBuffer.clear();
    _partialLine.clear();
    _lastCharWasCr = false;
}

int Console::addListener(OutputCallback callback)
{
    std::lock_guard<std::mutex> lock(_listenersMutex);
    int id = _nextListenerId++;
    _listeners.emplace_back(id, std::move(callback));
    return id;
}

void Console::removeListener(int listenerId)
{
    std::lock_guard<std::mutex> lock(_listenersMutex);
    _listeners.erase(
        std::remove_if(_listeners.begin(), _listeners.end(),
                       [listenerId](const std::pair<int, OutputCallback> &item) {
                           return item.first == listenerId;
                       }),
        _listeners.end());
}

const std::string &Console::getName() const
{
    return _name;
}

const std::string &Console::getPort() const
{
    return _port;
}

int Console::getBaudRate() const
{
    return _baudRate;
}

int Console::getTelnetPort() const
{
    return _telnetPort;
}

void Console::readWorker()
{
    char buf[512];

    while (_running) {
        if (!_serial.isOpen()) {
            if (_serial.open(_port, _baudRate)) {
                _connected = true;
            } else {
                _connected = false;
                std::this_thread::sleep_for(std::chrono::milliseconds(1000));
                continue;
            }
        }

        ssize_t n = _serial.read(buf, sizeof(buf), 100);
        if (n > 0) {
            // Broadcast raw bytes to all listeners (e.g. Telnet clients)
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
                        cb(buf, static_cast<size_t>(n));
                    }
                }
            }

            // Buffer lines for web UI / logging
            {
                std::lock_guard<std::mutex> lock(_lineMutex);
                for (ssize_t i = 0; i < n; ++i) {
                    char c = buf[i];
                    if (c == '\n') {
                        if (_lastCharWasCr) {
                            // \r was already handled; ignore matching \n in \r\n pair
                            _lastCharWasCr = false;
                        } else {
                            // Standalone \n
                            _lineBuffer.push(_partialLine);
                            _partialLine.clear();
                        }
                    } else if (c == '\r') {
                        if (!_lastCharWasCr) {
                            _lineBuffer.push(_partialLine);
                            _partialLine.clear();
                            _lastCharWasCr = true;
                        }
                    } else {
                        _lastCharWasCr = false;
                        _partialLine += c;
                        // Avoid unbounded partial line growth if no newline occurs
                        if (_partialLine.size() > 4096) {
                            _lineBuffer.push(_partialLine);
                            _partialLine.clear();
                        }
                    }
                }
            }
        } else if (n < 0) {
            // Read error or device disconnected
            _connected = false;
            _serial.close();
            std::this_thread::sleep_for(std::chrono::milliseconds(1000));
        }
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
