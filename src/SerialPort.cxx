/*
 * SerialPort.cxx
 *
 * Copyright (C) 2026, Charles Chiou
 */

#include "SerialPort.hxx"
#include <fcntl.h>
#include <unistd.h>
#include <poll.h>
#include <dirent.h>
#include <limits.h>
#include <cstring>
#include <cstdio>
#include <cerrno>
#include <chrono>
#include <vector>
#include <algorithm>

namespace embdevenv {

SerialPort::SerialPort()
    : _device("")
    , _usbPath("")
    , _portIndex(-1)
    , _baudRate(115200)
    , _fd(-1)
    , _lastOpenError(0)
    , _lastOpenLogMs(0)
{
}

SerialPort::SerialPort(const std::string &device, int baudRate)
    : _device(device)
    , _usbPath("")
    , _portIndex(-1)
    , _baudRate(baudRate)
    , _fd(-1)
    , _lastOpenError(0)
    , _lastOpenLogMs(0)
{
}

SerialPort::~SerialPort()
{
    close();
}

std::vector<std::string> SerialPort::resolveUsbTtyNodes(const std::string &usbPath)
{
    std::vector<std::string> results;
    if (usbPath.empty()) {
        return results;
    }

    DIR *dir = opendir("/sys/class/tty");
    if (!dir) {
        return results;
    }

    struct NodeInfo {
        std::string devPath;
        int minor;
    };
    std::vector<NodeInfo> matchedNodes;

    struct dirent *entry;
    while ((entry = readdir(dir)) != nullptr) {
        if (entry->d_name[0] == '.') {
            continue;
        }

        std::string name = entry->d_name;
        // Only inspect USB serial tty class devices
        if (name.rfind("ttyCH9344USB", 0) != 0 &&
            name.rfind("ttyUSB", 0) != 0 &&
            name.rfind("ttyACM", 0) != 0) {
            continue;
        }

        std::string devLink = "/sys/class/tty/" + name + "/device";
        char resolved[PATH_MAX];
        char *rp = realpath(devLink.c_str(), resolved);
        if (!rp) {
            continue;
        }

        std::string resolvedPath(rp);
        // Check if resolved sysfs path contains usbPath (e.g. "4-10.3.2")
        if (resolvedPath.find(usbPath) == std::string::npos) {
            continue;
        }

        // Read minor number from /sys/class/tty/<name>/dev
        int major = 0, minor = 0;
        std::string devFile = "/sys/class/tty/" + name + "/dev";
        FILE *df = fopen(devFile.c_str(), "r");
        if (df) {
            if (fscanf(df, "%d:%d", &major, &minor) == 2) {
                matchedNodes.push_back({"/dev/" + name, minor});
            }
            fclose(df);
        }
    }
    closedir(dir);

    // Sort by minor number in ascending order
    std::sort(matchedNodes.begin(), matchedNodes.end(), [](const NodeInfo &a, const NodeInfo &b) {
        return a.minor < b.minor;
    });

    for (const auto &node : matchedNodes) {
        results.push_back(node.devPath);
    }

    return results;
}

void SerialPort::setUsbBinding(const std::string &usbPath, int portIndex)
{
    std::scoped_lock lock(_lifecycleMutex);
    _usbPath = usbPath;
    _portIndex = portIndex;
}

const std::string &SerialPort::getUsbPath() const
{
    return _usbPath;
}

int SerialPort::getPortIndex() const
{
    return _portIndex;
}

bool SerialPort::open()
{
    return open(_device, _baudRate);
}

bool SerialPort::open(const std::string &device, int baudRate)
{
    std::scoped_lock lock(_lifecycleMutex, _readMutex, _writeMutex);

    if (_fd >= 0) {
        ::close(_fd);
        _fd = -1;
    }

    _device = device;
    _baudRate = baudRate;
    _readRemainder.clear();

    // If device is not accessible or empty, attempt dynamic resolution via USB topology binding
    if ((_device.empty() || access(_device.c_str(), F_OK) != 0) && !_usbPath.empty() && _portIndex >= 0) {
        auto nodes = resolveUsbTtyNodes(_usbPath);
        if (_portIndex < static_cast<int>(nodes.size())) {
            std::string resolved = nodes[_portIndex];
            if (resolved != _device) {
                fprintf(stderr, "[embdevenv] SerialPort: re-bound from %s to %s (index %d on usb %s)\n",
                        _device.empty() ? "(empty)" : _device.c_str(),
                        resolved.c_str(), _portIndex, _usbPath.c_str());
                _device = resolved;
            }
        }
    }

    if (_device.empty()) {
        fprintf(stderr, "[embdevenv] SerialPort: empty device path\n");
        return false;
    }

    _fd = ::open(_device.c_str(), O_RDWR | O_NOCTTY | O_NONBLOCK);
    if (_fd < 0) {
        int err = errno;
        auto now = std::chrono::steady_clock::now();
        uint64_t nowMs = std::chrono::duration_cast<std::chrono::milliseconds>(
            now.time_since_epoch()).count();

        // Rate-limit identical error logs to once every 10 seconds
        if (err != _lastOpenError || (nowMs - _lastOpenLogMs) >= 10000) {
            fprintf(stderr, "[embdevenv] SerialPort: failed to open %s: %s (retrying in background)\n",
                    _device.c_str(), strerror(err));
            _lastOpenError = err;
            _lastOpenLogMs = nowMs;
        }
        return false;
    }

    if (!configureTermios()) {
        ::close(_fd);
        _fd = -1;
        return false;
    }

    if (_lastOpenError != 0) {
        fprintf(stderr, "[embdevenv] SerialPort: connected to %s successfully\n", _device.c_str());
        _lastOpenError = 0;
    }

    return true;
}

void SerialPort::close()
{
    std::scoped_lock lock(_lifecycleMutex, _readMutex, _writeMutex);
    if (_fd >= 0) {
        ::close(_fd);
        _fd = -1;
    }
    _readRemainder.clear();
}

bool SerialPort::isOpen() const
{
    std::lock_guard<std::mutex> lock(_lifecycleMutex);
    return _fd >= 0;
}

const std::string &SerialPort::getDevice() const
{
    return _device;
}

int SerialPort::getBaudRate() const
{
    return _baudRate;
}

speed_t SerialPort::baudRateToSpeed(int baudRate) const
{
    switch (baudRate) {
    case 9600:
        return B9600;
    case 19200:
        return B19200;
    case 38400:
        return B38400;
    case 57600:
        return B57600;
    case 115200:
        return B115200;
    case 230400:
        return B230400;
    case 460800:
        return B460800;
    case 921600:
        return B921600;
    default:
        fprintf(stderr, "[embdevenv] SerialPort: unsupported baud rate %d, defaulting to 115200\n",
                baudRate);
        return B115200;
    }
}

bool SerialPort::configureTermios()
{
    struct termios options;
    memset(&options, 0, sizeof(options));

    if (tcgetattr(_fd, &options) != 0) {
        fprintf(stderr, "[embdevenv] SerialPort: tcgetattr failed for %s: %s\n",
                _device.c_str(), strerror(errno));
        return false;
    }

    cfmakeraw(&options);

    speed_t speed = baudRateToSpeed(_baudRate);
    cfsetispeed(&options, speed);
    cfsetospeed(&options, speed);

    options.c_cflag |= (CLOCAL | CREAD);
    options.c_cflag &= ~PARENB;
    options.c_cflag &= ~CSTOPB;
    options.c_cflag &= ~CSIZE;
    options.c_cflag |= CS8;
    options.c_cflag &= ~CRTSCTS; // No hardware flow control

    options.c_cc[VMIN] = 0;
    options.c_cc[VTIME] = 0;

    tcflush(_fd, TCIFLUSH);
    if (tcsetattr(_fd, TCSANOW, &options) != 0) {
        fprintf(stderr, "[embdevenv] SerialPort: tcsetattr failed for %s: %s\n",
                _device.c_str(), strerror(errno));
        return false;
    }

    return true;
}

ssize_t SerialPort::read(char *buf, size_t maxLen, int timeoutMs)
{
    int fd = -1;
    {
        std::lock_guard<std::mutex> lock(_lifecycleMutex);
        fd = _fd;
    }
    if (fd < 0 || buf == nullptr || maxLen == 0) {
        return -1;
    }

    struct pollfd pfd;
    pfd.fd = fd;
    pfd.events = POLLIN;
    pfd.revents = 0;

    int ret = poll(&pfd, 1, timeoutMs);
    if (ret < 0) {
        return -1;
    }
    if (ret == 0) {
        return 0; // Timeout
    }

    if (pfd.revents & (POLLERR | POLLHUP | POLLNVAL)) {
        return -1;
    }

    if (pfd.revents & POLLIN) {
        std::lock_guard<std::mutex> lock(_readMutex);
        return ::read(fd, buf, maxLen);
    }

    return 0;
}

bool SerialPort::readLine(std::string &line, int timeoutMs)
{
    std::lock_guard<std::mutex> rlock(_readMutex);
    int fd = -1;
    {
        std::lock_guard<std::mutex> lock(_lifecycleMutex);
        fd = _fd;
    }
    if (fd < 0) {
        return false;
    }

    // Check if remainder already contains newline
    size_t nlPos = _readRemainder.find('\n');
    if (nlPos != std::string::npos) {
        line = _readRemainder.substr(0, nlPos);
        _readRemainder.erase(0, nlPos + 1);
        if (!line.empty() && line.back() == '\r') {
            line.pop_back();
        }
        return true;
    }

    char buf[256];
    int remainingTimeout = timeoutMs;
    const int stepMs = 50;

    while (remainingTimeout >= 0) {
        struct pollfd pfd;
        pfd.fd = fd;
        pfd.events = POLLIN;
        pfd.revents = 0;

        int ret = poll(&pfd, 1, (remainingTimeout < stepMs) ? remainingTimeout : stepMs);
        if (ret > 0 && (pfd.revents & POLLIN)) {
            ssize_t n = ::read(fd, buf, sizeof(buf) - 1);
            if (n > 0) {
                buf[n] = '\0';
                _readRemainder.append(buf, n);

                nlPos = _readRemainder.find('\n');
                if (nlPos != std::string::npos) {
                    line = _readRemainder.substr(0, nlPos);
                    _readRemainder.erase(0, nlPos + 1);
                    if (!line.empty() && line.back() == '\r') {
                        line.pop_back();
                    }
                    return true;
                }
            } else if (n < 0 && errno != EAGAIN && errno != EWOULDBLOCK) {
                return false;
            }
        } else if (ret < 0) {
            return false;
        }

        remainingTimeout -= stepMs;
    }

    return false;
}

ssize_t SerialPort::write(const char *data, size_t len)
{
    int fd = -1;
    {
        std::lock_guard<std::mutex> lock(_lifecycleMutex);
        fd = _fd;
    }
    if (fd < 0 || data == nullptr) {
        return -1;
    }
    if (len == 0) {
        return 0;
    }

    std::lock_guard<std::mutex> lock(_writeMutex);
    size_t totalWritten = 0;
    while (totalWritten < len) {
        struct pollfd pfd;
        pfd.fd = fd;
        pfd.events = POLLOUT;
        pfd.revents = 0;

        int ret = poll(&pfd, 1, 1000);
        if (ret <= 0) {
            return totalWritten > 0 ? (ssize_t)totalWritten : -1;
        }

        ssize_t n = ::write(fd, data + totalWritten, len - totalWritten);
        if (n > 0) {
            totalWritten += n;
        } else if (n < 0 && errno != EAGAIN && errno != EWOULDBLOCK) {
            return totalWritten > 0 ? (ssize_t)totalWritten : -1;
        }
    }

    return (ssize_t)totalWritten;
}

ssize_t SerialPort::write(const std::string &str)
{
    return write(str.data(), str.size());
}

void SerialPort::flush()
{
    int fd = -1;
    {
        std::lock_guard<std::mutex> lock(_lifecycleMutex);
        fd = _fd;
    }
    if (fd >= 0) {
        tcflush(fd, TCIOFLUSH);
    }
    std::scoped_lock lock(_readMutex, _writeMutex);
    _readRemainder.clear();
}

bool SerialPort::reconnect()
{
    close();
    return open();
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
