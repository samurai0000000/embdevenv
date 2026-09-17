/*
 * Console.hxx
 *
 * Copyright (C) 2026, Charles Chiou
 */

#ifndef EMBDEVENV_CONSOLE_HXX
#define EMBDEVENV_CONSOLE_HXX

#include <string>
#include <vector>
#include <thread>
#include <atomic>
#include <mutex>
#include <functional>
#include <memory>
#include "SerialPort.hxx"
#include "RingBuffer.hxx"
#include "TelnetBackend.hxx"

namespace embdevenv {

class Console : public ITelnetBackend {
public:
    Console(const std::string &name, const std::string &port, int baudRate = 115200,
            size_t lineBufferCapacity = 10000, int telnetPort = 0);
    Console(const std::string &port, int baudRate = 115200, size_t lineBufferCapacity = 10000);
    ~Console() override;

    bool start();
    void stop();
    bool isRunning() const;
    bool isConnected() const;

    ssize_t write(const char *data, size_t len) override;
    ssize_t write(const std::string &data) override;

    std::vector<std::string> getRecentLines(size_t count = 100) const;
    void clearBuffer();

    int addListener(OutputCallback callback) override;
    void removeListener(int listenerId) override;

    const std::string &getName() const;
    const std::string &getPort() const;
    int getBaudRate() const;
    int getTelnetPort() const;

private:
    void readWorker();

    std::string _name;
    std::string _port;
    int _baudRate;
    int _telnetPort;
    SerialPort _serial;

    std::atomic<bool> _running;
    std::atomic<bool> _connected;
    std::thread _workerThread;

    RingBuffer<std::string> _lineBuffer;
    std::string _partialLine;
    bool _lastCharWasCr;
    mutable std::mutex _lineMutex;

    mutable std::mutex _listenersMutex;
    int _nextListenerId;
    std::vector<std::pair<int, OutputCallback>> _listeners;
};

} // namespace embdevenv

#endif // EMBDEVENV_CONSOLE_HXX

/*
 * Local variables:
 * mode: C++
 * c-file-style: "BSD"
 * c-basic-offset: 4
 * tab-width: 4
 * indent-tabs-mode: nil
 * End:
 */
