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

namespace embdevenv {

class Console {
public:
    using OutputCallback = std::function<void(const char *data, size_t len)>;

    Console(const std::string &port, int baudRate = 115200, size_t lineBufferCapacity = 10000);
    ~Console();

    bool start();
    void stop();
    bool isRunning() const;
    bool isConnected() const;

    ssize_t write(const char *data, size_t len);
    ssize_t write(const std::string &data);

    std::vector<std::string> getRecentLines(size_t count = 100) const;
    void clearBuffer();

    int addListener(OutputCallback callback);
    void removeListener(int listenerId);

    const std::string &getPort() const;
    int getBaudRate() const;

private:
    void readWorker();

    std::string _port;
    int _baudRate;
    SerialPort _serial;

    std::atomic<bool> _running;
    std::atomic<bool> _connected;
    std::thread _workerThread;

    RingBuffer<std::string> _lineBuffer;
    std::string _partialLine;
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
