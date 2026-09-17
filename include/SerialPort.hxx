/*
 * SerialPort.hxx
 *
 * Copyright (C) 2026, Charles Chiou
 */

#ifndef EMBDEVENV_SERIAL_PORT_HXX
#define EMBDEVENV_SERIAL_PORT_HXX

#include <string>
#include <mutex>
#include <cstddef>
#include <termios.h>

namespace embdevenv {

class SerialPort {
public:
    SerialPort();
    explicit SerialPort(const std::string &device, int baudRate = 115200);
    ~SerialPort();

    // Prevent copying
    SerialPort(const SerialPort &) = delete;
    SerialPort &operator=(const SerialPort &) = delete;

    bool open();
    bool open(const std::string &device, int baudRate);
    void close();
    bool isOpen() const;

    const std::string &getDevice() const;
    int getBaudRate() const;

    ssize_t read(char *buf, size_t maxLen, int timeoutMs = 100);
    bool readLine(std::string &line, int timeoutMs = 1000);
    ssize_t write(const char *data, size_t len);
    ssize_t write(const std::string &str);
    void flush();

    // Reconnection helper
    bool reconnect();

private:
    speed_t baudRateToSpeed(int baudRate) const;
    bool configureTermios();

    std::string _device;
    int _baudRate;
    int _fd;
    mutable std::mutex _lifecycleMutex;
    mutable std::mutex _readMutex;
    mutable std::mutex _writeMutex;
    std::string _readRemainder;
    int _lastOpenError;
    uint64_t _lastOpenLogMs;
};

} // namespace embdevenv

#endif // EMBDEVENV_SERIAL_PORT_HXX

/*
 * Local variables:
 * mode: C++
 * c-file-style: "BSD"
 * c-basic-offset: 4
 * tab-width: 4
 * indent-tabs-mode: nil
 * End:
 */
