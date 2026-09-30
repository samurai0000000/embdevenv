/*
 * UsageDatabase.hxx
 *
 * Copyright (C) 2026, Charles Chiou
 */

#ifndef EMBDEVENV_USAGE_DATABASE_HXX
#define EMBDEVENV_USAGE_DATABASE_HXX

#include <string>
#include <vector>
#include <map>
#include <mutex>
#include <thread>
#include <atomic>
#include <cstdint>
#include <nlohmann/json.hpp>
#include <sqlite3.h>

namespace embdevenv {

struct BoardEvent {
    int64_t timestamp = 0;
    std::string boardName;
    std::string eventType; // "flash", "reboot", "power_on", "power_off", "usb_download", "bootloader_catch"
    std::string status;    // "success", "failed", "timeout"
    int64_t durationMs = 0;
    std::string details;
};

class UsageDatabase {
public:
    static UsageDatabase &getInstance();

    bool open(const std::string &dbPath = "");
    void close();
    bool isOpen() const;

    // Ingestion
    bool recordEvent(const std::string &boardName,
                     const std::string &eventType,
                     const std::string &status,
                     int64_t durationMs = 0,
                     const std::string &details = "");

    bool recordTelemetrySample(const std::string &boardName,
                               bool powerOn,
                               double tempC,
                               int fanRpm,
                               const std::map<std::string, double> &voltages,
                               double maxTempC = 0.0,
                               const std::map<std::string, double> &minVoltages = {},
                               bool faultDetected = false);

    // Queries
    nlohmann::json queryBoardStats(const std::string &boardName, int windowDays = 30);
    nlohmann::json queryRecentEvents(const std::string &boardName, int limit = 50);
    int getPragmaInt(const std::string &pragmaSql) const;

    // Maintenance / Rollups
    void performRollupAndPrune(int rawRetentionDays = 90, int rollupRetentionDays = 365);

private:
    UsageDatabase();
    ~UsageDatabase();
    UsageDatabase(const UsageDatabase &) = delete;
    UsageDatabase &operator=(const UsageDatabase &) = delete;

    bool initSchema();
    void maintenanceWorker();

    std::string _dbPath;
    sqlite3 *_db;
    mutable std::mutex _mutex;

    std::atomic<bool> _running;
    std::thread _maintenanceThread;
};

} // namespace embdevenv

#endif // EMBDEVENV_USAGE_DATABASE_HXX

/*
 * Local variables:
 * mode: C++
 * c-file-style: "BSD"
 * c-basic-offset: 4
 * tab-width: 4
 * indent-tabs-mode: nil
 * End:
 */
