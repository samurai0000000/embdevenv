/*
 * TargetManager.hxx
 *
 * Copyright (C) 2026, Charles Chiou
 */

#ifndef EMBDEVENV_TARGET_MANAGER_HXX
#define EMBDEVENV_TARGET_MANAGER_HXX

#include <string>
#include <map>
#include <vector>
#include <memory>
#include <mutex>
#include <nlohmann/json.hpp>
#include "Target.hxx"

namespace embdevenv {

class TargetManager {
public:
    static TargetManager &getInstance();

    void addTarget(std::shared_ptr<Target> target);
    std::shared_ptr<Target> getTarget(const std::string &name) const;
    std::vector<std::shared_ptr<Target>> getAllTargets() const;

    void startAll();
    void stopAll();

    nlohmann::json getStatusJson() const;

private:
    TargetManager() = default;
    ~TargetManager() = default;
    TargetManager(const TargetManager &) = delete;
    TargetManager &operator=(const TargetManager &) = delete;

    mutable std::mutex _mutex;
    std::map<std::string, std::shared_ptr<Target>> _targets;
};

} // namespace embdevenv

#endif // EMBDEVENV_TARGET_MANAGER_HXX

/*
 * Local variables:
 * mode: C++
 * c-file-style: "BSD"
 * c-basic-offset: 4
 * tab-width: 4
 * indent-tabs-mode: nil
 * End:
 */
