/*
 * TelemetryStore.cxx
 *
 * Copyright (C) 2026, Charles Chiou
 */

#include "TelemetryStore.hxx"
#include <chrono>
#include <iomanip>
#include <sstream>

namespace embdevenv {

TelemetrySample::TelemetrySample()
    : timestampMs(0)
    , timestampIso("")
    , powerOn(false)
    , temperatureC(0.0)
    , fanRpm(0)
    , raw("")
{
}

nlohmann::json TelemetrySample::toJson() const
{
    nlohmann::json j;
    j["timestampMs"] = timestampMs;
    j["timestampIso"] = timestampIso;
    j["powerOn"] = powerOn;
    j["temperatureC"] = temperatureC;
    j["fanRpm"] = fanRpm;
    j["voltages"] = voltages;
    j["raw"] = raw;
    return j;
}

TelemetryStore::TelemetryStore(size_t historyCapacity)
    : _hasLatest(false)
    , _history(historyCapacity)
{
}

void TelemetryStore::addSample(const TelemetrySample &sample)
{
    std::lock_guard<std::mutex> lock(_mutex);
    _latestSample = sample;
    _hasLatest = true;
    _history.push(sample);
}

bool TelemetryStore::getLatest(TelemetrySample &sample) const
{
    std::lock_guard<std::mutex> lock(_mutex);
    if (!_hasLatest) {
        return false;
    }
    sample = _latestSample;
    return true;
}

std::vector<TelemetrySample> TelemetryStore::getHistory(size_t count) const
{
    return _history.getLatest(count);
}

void TelemetryStore::clear()
{
    std::lock_guard<std::mutex> lock(_mutex);
    _hasLatest = false;
    _latestSample = TelemetrySample();
    _history.clear();
}

nlohmann::json TelemetryStore::toJson(size_t historyCount) const
{
    nlohmann::json root;

    TelemetrySample latest;
    if (getLatest(latest)) {
        root["latest"] = latest.toJson();
    } else {
        root["latest"] = nullptr;
    }

    nlohmann::json historyArr = nlohmann::json::array();
    std::vector<TelemetrySample> samples = getHistory(historyCount);
    for (const auto &s : samples) {
        historyArr.push_back(s.toJson());
    }
    root["history"] = historyArr;
    root["count"] = samples.size();

    return root;
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
