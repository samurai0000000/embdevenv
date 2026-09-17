/*
 * N1655Target.hxx
 *
 * Copyright (C) 2026, Charles Chiou
 */

#ifndef EMBDEVENV_N1655_TARGET_HXX
#define EMBDEVENV_N1655_TARGET_HXX

#include "Target.hxx"
#include "N1655Mcu.hxx"
#include "TargetFactory.hxx"

namespace embdevenv {

class N1655Target : public Target {
public:
    explicit N1655Target(const TargetConfig &config);
    N1655Target(const std::string &name,
                const std::string &model,
                const std::string &description,
                bool enabled,
                int telnetPort,
                const std::string &consolePort,
                int consoleBaudRate,
                const std::string &mcuPort,
                int mcuBaudRate,
                int mcuPollIntervalMs);
    ~N1655Target() override = default;
};

} // namespace embdevenv

#endif // EMBDEVENV_N1655_TARGET_HXX

/*
 * Local variables:
 * mode: C++
 * c-file-style: "BSD"
 * c-basic-offset: 4
 * tab-width: 4
 * indent-tabs-mode: nil
 * End:
 */
