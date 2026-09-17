/*
 * TelnetBackend.hxx
 *
 * Copyright (C) 2026, Charles Chiou
 */

#ifndef EMBDEVENV_TELNET_BACKEND_HXX
#define EMBDEVENV_TELNET_BACKEND_HXX

#include <string>
#include <functional>
#include <cstddef>
#include <sys/types.h>

namespace embdevenv {

class ITelnetBackend {
public:
    using OutputCallback = std::function<void(const char *data, size_t len)>;

    virtual ~ITelnetBackend() = default;
    virtual ssize_t write(const char *data, size_t len) = 0;
    virtual ssize_t write(const std::string &data) = 0;
    virtual int addListener(OutputCallback callback) = 0;
    virtual void removeListener(int listenerId) = 0;

    virtual void onClientConnect() {}
    virtual void onClientDisconnect() {}
};

} // namespace embdevenv

#endif // EMBDEVENV_TELNET_BACKEND_HXX

/*
 * Local variables:
 * mode: C++
 * c-file-style: "BSD"
 * c-basic-offset: 4
 * tab-width: 4
 * indent-tabs-mode: nil
 * End:
 */
