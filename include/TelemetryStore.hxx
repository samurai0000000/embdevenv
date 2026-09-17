/*
 * TelemetryStore.hxx
 *
 * Copyright (C) 2026, Charles Chiou
 */

#ifndef EMBDEVENV_TELEMETRY_STORE_HXX
#define EMBDEVENV_TELEMETRY_STORE_HXX

#include <string>
#include <map>
#include <vector>
#include <mutex>
#include <cstdint>
#include <nlohmann/json.hpp>
#include "RingBuffer.hxx"

namespace embdevenv {

struct TelemetrySample {
    uint64_t timestampMs;
    std::string timestampIso;
    bool powerOn;
    double temperatureC;
    int fanRpm;
    std::map<std::string, double> voltages;
    std::string raw;

    TelemetrySample();
    nlohmann::json toJson() const;
};

class TelemetryStore {
public:
    explicit TelemetryStore(size_t historyCapacity = 1000);
    ~TelemetryStore() = default;

    void addSample(const TelemetrySample &sample);
    bool getLatest(TelemetrySample &sample) const;
    std::vector<TelemetrySample> getHistory(size_t count = 100) const;
    void clear();

    nlohmann::json toJson(size_t historyCount = 100) const;

private:
    mutable std::mutex _mutex;
    bool _hasLatest;
    TelemetrySample _latestSample;
    RingBuffer<TelemetrySample> _history;
};

} // namespace embdevenv

#endif // EMBDEVENV_TELEMETRY_STORE_HXX

/*
 * Local variables:
 * mode: C++
 * c-file-style: "BSD"
 * c-basic-offset: 4
 * tab-width: 4
 * indent-tabs-mode: nil
 * End:
 */
