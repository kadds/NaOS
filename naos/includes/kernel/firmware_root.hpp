#pragma once

#include "kernel/arch/acpi/boot_tables.hpp"
#include "kernel/kobject.hpp"

namespace firmware
{
class root final : public kobject
{
  public:
    static constexpr type_e type_of() { return type_e::firmware_root; }

    explicit root(const arch::acpi_boot::snapshot *snapshot)
        : kobject(type_of())
        , snapshot_(snapshot)
    {
    }

    const arch::acpi_boot::snapshot *snapshot() const { return snapshot_; }
    bool capability_is_unique() const override { return false; }
    na_signal_t capability_signals() const override { return NA_SIGNAL_WRITABLE; }

  private:
    const arch::acpi_boot::snapshot *snapshot_;
};
} // namespace firmware
