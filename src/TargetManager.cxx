/*
 * TargetManager.cxx
 *
 * Copyright (C) 2026, Charles Chiou
 */

#include "TargetManager.hxx"

namespace embdevenv {

TargetManager &TargetManager::getInstance()
{
    static TargetManager instance;
    return instance;
}

void TargetManager::addTarget(std::shared_ptr<Target> target)
{
    if (target == nullptr) {
        return;
    }

    std::lock_guard<std::mutex> lock(_mutex);
    _targets[target->getName()] = target;
}

std::shared_ptr<Target> TargetManager::getTarget(const std::string &name) const
{
    std::lock_guard<std::mutex> lock(_mutex);
    auto it = _targets.find(name);
    if (it != _targets.end()) {
        return it->second;
    }
    return nullptr;
}

std::vector<std::shared_ptr<Target>> TargetManager::getAllTargets() const
{
    std::lock_guard<std::mutex> lock(_mutex);
    std::vector<std::shared_ptr<Target>> result;
    result.reserve(_targets.size());
    for (const auto &pair : _targets) {
        result.push_back(pair.second);
    }
    return result;
}

void TargetManager::startAll()
{
    std::vector<std::shared_ptr<Target>> targets = getAllTargets();
    for (auto &target : targets) {
        target->start();
    }
}

void TargetManager::stopAll()
{
    std::vector<std::shared_ptr<Target>> targets = getAllTargets();
    for (auto &target : targets) {
        target->stop();
    }
}

nlohmann::json TargetManager::getStatusJson() const
{
    nlohmann::json j;
    nlohmann::json targetsArr = nlohmann::json::array();

    std::vector<std::shared_ptr<Target>> targets = getAllTargets();
    for (const auto &target : targets) {
        targetsArr.push_back(target->getStatusJson());
    }

    j["targets"] = targetsArr;
    j["count"] = targets.size();
    return j;
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
