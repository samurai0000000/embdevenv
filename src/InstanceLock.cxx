/*
 * InstanceLock.cxx
 *
 * Copyright (C) 2026, Charles Chiou
 */

#include "InstanceLock.hxx"
#include "Config.hxx"
#include <unistd.h>
#include <fcntl.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <signal.h>
#include <cstring>
#include <cstdio>
#include <fstream>

namespace embdevenv {

InstanceLock::InstanceLock()
    : _lockFd(-1)
    , _existingPid(0)
    , _locked(false)
{
    char buf[256];
    if (gethostname(buf, sizeof(buf)) == 0) {
        buf[sizeof(buf) - 1] = '\0';
        char *dot = strchr(buf, '.');
        if (dot != nullptr) {
            *dot = '\0';
        }
        _hostname = buf;
    } else {
        _hostname = "localhost";
    }
}

InstanceLock::~InstanceLock()
{
    release();
}

bool InstanceLock::acquire(const std::string &lockDir)
{
    std::string dir = lockDir;
    if (dir.empty()) {
        dir = Config::resolveHomePath("~/.config/embdevenv");
    }

    if (mkdir(dir.c_str(), 0755) != 0 && errno != EEXIST) {
        dir = "/tmp";
    }

    _lockFilePath = dir + "/embdevenv." + _hostname + ".pid";

    // 1. Check existing lock file content
    std::ifstream ifs(_lockFilePath);
    if (ifs.is_open()) {
        pid_t oldPid = 0;
        std::string recordedHost;
        if (ifs >> oldPid) {
            ifs >> recordedHost;
            // Only validate against local processes if the file recorded this host
            if (recordedHost.empty() || recordedHost == _hostname) {
                if (oldPid > 0 && (kill(oldPid, 0) == 0 || errno == EPERM)) {
                    // Verify process name in /proc if available
                    bool isSame = true;
                    std::string procPath = "/proc/" + std::to_string(oldPid) + "/comm";
                    std::ifstream procIfs(procPath);
                    if (procIfs.is_open()) {
                        std::string comm;
                        procIfs >> comm;
                        if (!comm.empty() && comm.find("embdevenv") == std::string::npos) {
                            isSame = false; // Process ID recycled by an unrelated process
                        }
                    }

                    if (isSame) {
                        _existingPid = oldPid;
                        return false;
                    }
                }
            }
        }
        ifs.close();
    }

    // 2. Open / create lock file
    _lockFd = open(_lockFilePath.c_str(), O_RDWR | O_CREAT, 0644);
    if (_lockFd < 0) {
        fprintf(stderr, "[embdevenv] Failed to open lock file %s: %s\n",
                _lockFilePath.c_str(), strerror(errno));
        return false;
    }

    // 3. Acquire POSIX write lock
    struct flock fl;
    std::memset(&fl, 0, sizeof(fl));
    fl.l_type = F_WRLCK;
    fl.l_whence = SEEK_SET;
    fl.l_start = 0;
    fl.l_len = 0;

    if (fcntl(_lockFd, F_SETLK, &fl) < 0) {
        if (errno == EACCES || errno == EAGAIN) {
            std::ifstream ifsCheck(_lockFilePath);
            pid_t p = 0;
            if (ifsCheck >> p && p > 0) {
                _existingPid = p;
            }
            close(_lockFd);
            _lockFd = -1;
            return false;
        }
    }

    // 4. Truncate and write our PID and hostname
    if (ftruncate(_lockFd, 0) != 0) {
        // Ignore ftruncate error
    }
    lseek(_lockFd, 0, SEEK_SET);

    pid_t currentPid = getpid();
    std::string lockContent = std::to_string(currentPid) + "\n" + _hostname + "\n";
    ssize_t written = write(_lockFd, lockContent.c_str(), lockContent.size());
    (void)written;
    fsync(_lockFd);

    _locked = true;
    return true;
}

void InstanceLock::release()
{
    if (_locked) {
        _locked = false;
        if (_lockFd >= 0) {
            struct flock fl;
            std::memset(&fl, 0, sizeof(fl));
            fl.l_type = F_UNLCK;
            fl.l_whence = SEEK_SET;
            fl.l_start = 0;
            fl.l_len = 0;
            fcntl(_lockFd, F_SETLK, &fl);

            close(_lockFd);
            _lockFd = -1;
        }
        if (!_lockFilePath.empty()) {
            unlink(_lockFilePath.c_str());
        }
    }
}

const std::string &InstanceLock::getHostname() const
{
    return _hostname;
}

const std::string &InstanceLock::getLockFilePath() const
{
    return _lockFilePath;
}

pid_t InstanceLock::getExistingPid() const
{
    return _existingPid;
}

bool InstanceLock::isLocked() const
{
    return _locked;
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
