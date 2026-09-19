/*
 * UsageDatabase.cxx
 *
 * Copyright (C) 2026, Charles Chiou
 */

#include "UsageDatabase.hxx"
#include "Config.hxx"
#include <chrono>
#include <iostream>
#include <sstream>
#include <filesystem>
#include <ctime>

namespace fs = std::filesystem;

namespace embdevenv {

UsageDatabase &UsageDatabase::getInstance()
{
    static UsageDatabase instance;
    return instance;
}

UsageDatabase::UsageDatabase()
    : _db(nullptr)
    , _running(false)
{
}

UsageDatabase::~UsageDatabase()
{
    close();
}

bool UsageDatabase::open(const std::string &dbPath)
{
    std::lock_guard<std::mutex> lock(_mutex);
    if (_db != nullptr) {
        return true;
    }

    if (!dbPath.empty()) {
        _dbPath = Config::resolveHomePath(dbPath);
    } else {
        _dbPath = Config::resolveHomePath(Config::getInstance().getStatsConfig().dbPath);
    }

    try {
        fs::path p(_dbPath);
        if (p.has_parent_path()) {
            fs::create_directories(p.parent_path());
        }
    } catch (const std::exception &ex) {
        std::cerr << "[UsageDatabase] Failed to create database directory: "
                  << ex.what() << std::endl;
    }

    int rc = sqlite3_open(_dbPath.c_str(), &_db);
    if (rc != SQLITE_OK) {
        std::cerr << "[UsageDatabase] Failed to open SQLite database "
                  << _dbPath << ": " << sqlite3_errmsg(_db) << std::endl;
        if (_db) {
            sqlite3_close(_db);
            _db = nullptr;
        }
        return false;
    }

    char *err = nullptr;
    sqlite3_exec(_db, "PRAGMA journal_mode=WAL;", nullptr, nullptr, &err);
    if (err) sqlite3_free(err);
    sqlite3_exec(_db, "PRAGMA synchronous=NORMAL;", nullptr, nullptr, &err);
    if (err) sqlite3_free(err);

    if (!initSchema()) {
        std::cerr << "[UsageDatabase] Failed to initialize schema" << std::endl;
        sqlite3_close(_db);
        _db = nullptr;
        return false;
    }

    _running.store(true);
    _maintenanceThread = std::thread(&UsageDatabase::maintenanceWorker, this);

    std::cout << "[UsageDatabase] Opened " << _dbPath << " successfully (WAL mode)" << std::endl;
    return true;
}

void UsageDatabase::close()
{
    if (_running.load()) {
        _running.store(false);
        if (_maintenanceThread.joinable()) {
            _maintenanceThread.join();
        }
    }

    std::lock_guard<std::mutex> lock(_mutex);
    if (_db != nullptr) {
        sqlite3_close(_db);
        _db = nullptr;
    }
}

bool UsageDatabase::isOpen() const
{
    std::lock_guard<std::mutex> lock(_mutex);
    return _db != nullptr;
}

bool UsageDatabase::initSchema()
{
    const char *ddlEvents =
        "CREATE TABLE IF NOT EXISTS board_events ("
        "  id INTEGER PRIMARY KEY AUTOINCREMENT,"
        "  timestamp INTEGER NOT NULL,"
        "  board_name TEXT NOT NULL,"
        "  event_type TEXT NOT NULL,"
        "  status TEXT NOT NULL,"
        "  duration_ms INTEGER DEFAULT 0,"
        "  details TEXT DEFAULT ''"
        ");"
        "CREATE INDEX IF NOT EXISTS idx_board_events_lookup "
        "ON board_events(board_name, event_type, timestamp);";

    const char *ddlTelemetry =
        "CREATE TABLE IF NOT EXISTS board_telemetry_samples ("
        "  timestamp INTEGER NOT NULL,"
        "  board_name TEXT NOT NULL,"
        "  power_on INTEGER NOT NULL,"
        "  temp_c REAL NOT NULL,"
        "  fan_rpm INTEGER NOT NULL,"
        "  voltages_json TEXT DEFAULT ''"
        ");"
        "CREATE INDEX IF NOT EXISTS idx_board_telemetry_lookup "
        "ON board_telemetry_samples(board_name, timestamp);";

    const char *ddlHourly =
        "CREATE TABLE IF NOT EXISTS board_hourly_rollups ("
        "  timestamp INTEGER NOT NULL,"
        "  board_name TEXT NOT NULL,"
        "  power_on_seconds REAL DEFAULT 0.0,"
        "  reboot_count INTEGER DEFAULT 0,"
        "  power_cycle_count INTEGER DEFAULT 0,"
        "  flash_count INTEGER DEFAULT 0,"
        "  flash_success_count INTEGER DEFAULT 0,"
        "  usb_download_count INTEGER DEFAULT 0,"
        "  avg_temp_c REAL DEFAULT 0.0,"
        "  max_temp_c REAL DEFAULT 0.0,"
        "  min_temp_c REAL DEFAULT 0.0,"
        "  avg_fan_rpm REAL DEFAULT 0.0,"
        "  PRIMARY KEY (board_name, timestamp)"
        ");";

    const char *ddlDaily =
        "CREATE TABLE IF NOT EXISTS board_daily_rollups ("
        "  date TEXT NOT NULL,"
        "  board_name TEXT NOT NULL,"
        "  total_power_on_hours REAL DEFAULT 0.0,"
        "  total_reboots INTEGER DEFAULT 0,"
        "  total_power_cycles INTEGER DEFAULT 0,"
        "  total_flashes INTEGER DEFAULT 0,"
        "  total_flash_success INTEGER DEFAULT 0,"
        "  total_usb_downloads INTEGER DEFAULT 0,"
        "  avg_temp_c REAL DEFAULT 0.0,"
        "  max_temp_c REAL DEFAULT 0.0,"
        "  min_temp_c REAL DEFAULT 0.0,"
        "  avg_fan_rpm REAL DEFAULT 0.0,"
        "  PRIMARY KEY (board_name, date)"
        ");";

    char *err = nullptr;
    if (sqlite3_exec(_db, ddlEvents, nullptr, nullptr, &err) != SQLITE_OK) {
        std::cerr << "[UsageDatabase] DDL error (events): " << (err ? err : "") << std::endl;
        if (err) sqlite3_free(err);
        return false;
    }
    if (sqlite3_exec(_db, ddlTelemetry, nullptr, nullptr, &err) != SQLITE_OK) {
        std::cerr << "[UsageDatabase] DDL error (telemetry): " << (err ? err : "") << std::endl;
        if (err) sqlite3_free(err);
        return false;
    }
    if (sqlite3_exec(_db, ddlHourly, nullptr, nullptr, &err) != SQLITE_OK) {
        std::cerr << "[UsageDatabase] DDL error (hourly): " << (err ? err : "") << std::endl;
        if (err) sqlite3_free(err);
        return false;
    }
    if (sqlite3_exec(_db, ddlDaily, nullptr, nullptr, &err) != SQLITE_OK) {
        std::cerr << "[UsageDatabase] DDL error (daily): " << (err ? err : "") << std::endl;
        if (err) sqlite3_free(err);
        return false;
    }

    return true;
}

bool UsageDatabase::recordEvent(const std::string &boardName,
                                const std::string &eventType,
                                const std::string &status,
                                int64_t durationMs,
                                const std::string &details)
{
    if (boardName.empty()) {
        return false;
    }

    int64_t now = std::chrono::duration_cast<std::chrono::seconds>(
        std::chrono::system_clock::now().time_since_epoch()).count();

    std::lock_guard<std::mutex> lock(_mutex);
    if (!_db) {
        return false;
    }

    const char *sql =
        "INSERT INTO board_events (timestamp, board_name, event_type, status, duration_ms, details) "
        "VALUES (?, ?, ?, ?, ?, ?);";
    sqlite3_stmt *stmt = nullptr;
    if (sqlite3_prepare_v2(_db, sql, -1, &stmt, nullptr) != SQLITE_OK) {
        return false;
    }

    sqlite3_bind_int64(stmt, 1, now);
    sqlite3_bind_text(stmt, 2, boardName.c_str(), -1, SQLITE_TRANSIENT);
    sqlite3_bind_text(stmt, 3, eventType.c_str(), -1, SQLITE_TRANSIENT);
    sqlite3_bind_text(stmt, 4, status.c_str(), -1, SQLITE_TRANSIENT);
    sqlite3_bind_int64(stmt, 5, durationMs);
    sqlite3_bind_text(stmt, 6, details.c_str(), -1, SQLITE_TRANSIENT);

    int rc = sqlite3_step(stmt);
    sqlite3_finalize(stmt);
    return rc == SQLITE_DONE;
}

bool UsageDatabase::recordTelemetrySample(const std::string &boardName,
                                         bool powerOn,
                                         double tempC,
                                         int fanRpm,
                                         const std::map<std::string, double> &voltages)
{
    if (boardName.empty()) {
        return false;
    }

    int64_t now = std::chrono::duration_cast<std::chrono::seconds>(
        std::chrono::system_clock::now().time_since_epoch()).count();

    nlohmann::json vJson = voltages;
    std::string vStr = vJson.dump();

    std::lock_guard<std::mutex> lock(_mutex);
    if (!_db) {
        return false;
    }

    const char *sql =
        "INSERT INTO board_telemetry_samples (timestamp, board_name, power_on, temp_c, fan_rpm, voltages_json) "
        "VALUES (?, ?, ?, ?, ?, ?);";
    sqlite3_stmt *stmt = nullptr;
    if (sqlite3_prepare_v2(_db, sql, -1, &stmt, nullptr) != SQLITE_OK) {
        return false;
    }

    sqlite3_bind_int64(stmt, 1, now);
    sqlite3_bind_text(stmt, 2, boardName.c_str(), -1, SQLITE_TRANSIENT);
    sqlite3_bind_int(stmt, 3, powerOn ? 1 : 0);
    sqlite3_bind_double(stmt, 4, tempC);
    sqlite3_bind_int(stmt, 5, fanRpm);
    sqlite3_bind_text(stmt, 6, vStr.c_str(), -1, SQLITE_TRANSIENT);

    int rc = sqlite3_step(stmt);
    sqlite3_finalize(stmt);
    return rc == SQLITE_DONE;
}

nlohmann::json UsageDatabase::queryRecentEvents(const std::string &boardName, int limit)
{
    nlohmann::json result = nlohmann::json::array();
    std::lock_guard<std::mutex> lock(_mutex);
    if (!_db) {
        return result;
    }

    std::string sql =
        "SELECT timestamp, board_name, event_type, status, duration_ms, details "
        "FROM board_events ";
    if (!boardName.empty()) {
        sql += "WHERE board_name = ? ";
    }
    sql += "ORDER BY timestamp DESC LIMIT ?;";

    sqlite3_stmt *stmt = nullptr;
    if (sqlite3_prepare_v2(_db, sql.c_str(), -1, &stmt, nullptr) != SQLITE_OK) {
        return result;
    }

    int bindIdx = 1;
    if (!boardName.empty()) {
        sqlite3_bind_text(stmt, bindIdx++, boardName.c_str(), -1, SQLITE_TRANSIENT);
    }
    sqlite3_bind_int(stmt, bindIdx, limit > 0 ? limit : 50);

    while (sqlite3_step(stmt) == SQLITE_ROW) {
        nlohmann::json ev;
        int64_t ts = sqlite3_column_int64(stmt, 0);
        ev["timestamp"] = ts;
        ev["board_name"] = reinterpret_cast<const char *>(sqlite3_column_text(stmt, 1));
        ev["event_type"] = reinterpret_cast<const char *>(sqlite3_column_text(stmt, 2));
        ev["status"] = reinterpret_cast<const char *>(sqlite3_column_text(stmt, 3));
        ev["duration_ms"] = sqlite3_column_int64(stmt, 4);
        ev["details"] = reinterpret_cast<const char *>(sqlite3_column_text(stmt, 5));
        result.push_back(ev);
    }

    sqlite3_finalize(stmt);
    return result;
}

nlohmann::json UsageDatabase::queryBoardStats(const std::string &boardName, int windowDays)
{
    nlohmann::json root;
    root["target"] = boardName;
    root["window_days"] = windowDays;

    int64_t windowStart = 0;
    if (windowDays > 0) {
        int64_t now = std::chrono::duration_cast<std::chrono::seconds>(
            std::chrono::system_clock::now().time_since_epoch()).count();
        windowStart = now - (static_cast<int64_t>(windowDays) * 86400);
    }

    std::lock_guard<std::mutex> lock(_mutex);
    if (!_db) {
        root["error"] = "Database not open";
        return root;
    }

    // 1. Query event counts within window
    int flashTotal = 0;
    int flashSuccess = 0;
    int rebootsTotal = 0;
    int powerCyclesTotal = 0;
    int usbDownloadsTotal = 0;
    int bootloaderCatches = 0;

    const char *sqlEvents =
        "SELECT event_type, status, COUNT(*) "
        "FROM board_events "
        "WHERE board_name = ? AND timestamp >= ? "
        "GROUP BY event_type, status;";
    sqlite3_stmt *stmt = nullptr;
    if (sqlite3_prepare_v2(_db, sqlEvents, -1, &stmt, nullptr) == SQLITE_OK) {
        sqlite3_bind_text(stmt, 1, boardName.c_str(), -1, SQLITE_TRANSIENT);
        sqlite3_bind_int64(stmt, 2, windowStart);
        while (sqlite3_step(stmt) == SQLITE_ROW) {
            std::string type = reinterpret_cast<const char *>(sqlite3_column_text(stmt, 0));
            std::string status = reinterpret_cast<const char *>(sqlite3_column_text(stmt, 1));
            int count = sqlite3_column_int(stmt, 2);

            if (type == "flash") {
                flashTotal += count;
                if (status == "success") flashSuccess += count;
            } else if (type == "reboot") {
                rebootsTotal += count;
            } else if (type == "power_on" || type == "power_off") {
                powerCyclesTotal += count;
            } else if (type == "usb_download") {
                usbDownloadsTotal += count;
            } else if (type == "bootloader_catch") {
                bootloaderCatches += count;
            }
        }
        sqlite3_finalize(stmt);
    }

    // 2. Query telemetry aggregates (temperature & fan speed) within window
    double avgTemp = 0.0;
    double maxTemp = 0.0;
    double minTemp = 0.0;
    double avgFan = 0.0;
    int sampleCount = 0;

    const char *sqlTelem =
        "SELECT AVG(temp_c), MAX(temp_c), MIN(temp_c), AVG(fan_rpm), COUNT(*) "
        "FROM board_telemetry_samples "
        "WHERE board_name = ? AND timestamp >= ? AND temp_c > 0.0;";
    if (sqlite3_prepare_v2(_db, sqlTelem, -1, &stmt, nullptr) == SQLITE_OK) {
        sqlite3_bind_text(stmt, 1, boardName.c_str(), -1, SQLITE_TRANSIENT);
        sqlite3_bind_int64(stmt, 2, windowStart);
        if (sqlite3_step(stmt) == SQLITE_ROW && sqlite3_column_int(stmt, 4) > 0) {
            avgTemp = sqlite3_column_double(stmt, 0);
            maxTemp = sqlite3_column_double(stmt, 1);
            minTemp = sqlite3_column_double(stmt, 2);
            avgFan = sqlite3_column_double(stmt, 3);
            sampleCount = sqlite3_column_int(stmt, 4);
        }
        sqlite3_finalize(stmt);
    }

    // 3. Lifetime totals for events
    int lifetimeFlashes = 0;
    int lifetimeReboots = 0;
    int lifetimePowerCycles = 0;
    int lifetimeUsbDownloads = 0;
    int lifetimeBootloaderCatches = 0;
    const char *sqlLifetime =
        "SELECT event_type, COUNT(*) FROM board_events WHERE board_name = ? GROUP BY event_type;";
    if (sqlite3_prepare_v2(_db, sqlLifetime, -1, &stmt, nullptr) == SQLITE_OK) {
        sqlite3_bind_text(stmt, 1, boardName.c_str(), -1, SQLITE_TRANSIENT);
        while (sqlite3_step(stmt) == SQLITE_ROW) {
            std::string type = reinterpret_cast<const char *>(sqlite3_column_text(stmt, 0));
            int count = sqlite3_column_int(stmt, 1);
            if (type == "flash") lifetimeFlashes = count;
            else if (type == "reboot") lifetimeReboots = count;
            else if (type == "power_on" || type == "power_off") lifetimePowerCycles += count;
            else if (type == "usb_download") lifetimeUsbDownloads += count;
            else if (type == "bootloader_catch") lifetimeBootloaderCatches += count;
        }
        sqlite3_finalize(stmt);
    }

    nlohmann::json summary;
    summary["flashes_total"] = flashTotal;
    summary["flashes_success"] = flashSuccess;
    summary["flashes_failed"] = flashTotal - flashSuccess;
    summary["flash_success_rate"] = flashTotal > 0 ? (static_cast<double>(flashSuccess) / flashTotal) : 1.0;
    summary["reboots_total"] = rebootsTotal;
    summary["power_cycles_total"] = powerCyclesTotal;
    summary["usb_downloads_total"] = usbDownloadsTotal;
    summary["bootloader_catches_total"] = bootloaderCatches;
    summary["telemetry_samples_count"] = sampleCount;
    summary["avg_temp_c"] = avgTemp;
    summary["max_temp_c"] = maxTemp;
    summary["min_temp_c"] = minTemp;
    summary["avg_fan_rpm"] = avgFan;

    nlohmann::json lifetime;
    lifetime["flashes_total"] = lifetimeFlashes;
    lifetime["reboots_total"] = lifetimeReboots;
    lifetime["power_cycles_total"] = lifetimePowerCycles;
    lifetime["usb_downloads_total"] = lifetimeUsbDownloads;
    lifetime["bootloader_catches_total"] = lifetimeBootloaderCatches;

    root["summary"] = summary;
    root["lifetime_totals"] = lifetime;

    return root;
}

void UsageDatabase::performRollupAndPrune(int rawRetentionDays, int rollupRetentionDays)
{
    std::lock_guard<std::mutex> lock(_mutex);
    if (!_db) {
        return;
    }

    int64_t now = std::chrono::duration_cast<std::chrono::seconds>(
        std::chrono::system_clock::now().time_since_epoch()).count();
    int64_t rawCutoff = now - (static_cast<int64_t>(rawRetentionDays) * 86400);
    int64_t telemCutoff = now - (7 * 86400); // Retain detailed high-frequency telemetry samples for 7 days

    // Prune old raw telemetry samples
    std::string sqlPruneTelem = "DELETE FROM board_telemetry_samples WHERE timestamp < " + std::to_string(telemCutoff) + ";";
    sqlite3_exec(_db, sqlPruneTelem.c_str(), nullptr, nullptr, nullptr);

    // Prune old raw events
    std::string sqlPruneEvents = "DELETE FROM board_events WHERE timestamp < " + std::to_string(rawCutoff) + ";";
    sqlite3_exec(_db, sqlPruneEvents.c_str(), nullptr, nullptr, nullptr);
}

void UsageDatabase::maintenanceWorker()
{
    while (_running.load()) {
        for (int i = 0; i < 3600 && _running.load(); ++i) {
            std::this_thread::sleep_for(std::chrono::seconds(1));
        }
        if (!_running.load()) {
            break;
        }

        const auto &cfg = Config::getInstance().getStatsConfig();
        performRollupAndPrune(cfg.rawEventRetentionDays, cfg.rollupRetentionDays);
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
