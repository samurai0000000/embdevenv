/*
 * TargetFactory.cxx
 *
 * Copyright (C) 2026, Charles Chiou
 */

#include "TargetFactory.hxx"
#include "N1655Target.hxx"
#include <cstdio>

namespace embdevenv {

std::shared_ptr<Target> TargetFactory::createTarget(const TargetConfig &config)
{
    // Match N1-655 series (e.g. n1-655-pro, n1-655-devkit)
    if (config.model.rfind("n1-655", 0) == 0) {
        return std::make_shared<N1655Target>(config);
    }

    // Default fallback to N1-655 series or generic target
    fprintf(stderr, "[embdevenv] TargetFactory: unknown model '%s', defaulting to N1655Target\n",
            config.model.c_str());
    return std::make_shared<N1655Target>(config);
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
