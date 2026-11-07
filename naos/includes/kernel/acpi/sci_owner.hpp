#pragma once

#include "kernel/types.hpp"

namespace acpi
{
/// Single-owner, fail-closed lifecycle for a fixed-hardware authority.
/// SCI event execution and power transitions use separate instances so a
/// degraded AML event path does not revoke independent fixed-register control.
/// Callers serialize access; an exited owner is permanently revoked for this
/// boot so a restarted platformd cannot silently take over hardware state.
class fixed_hardware_owner_lifecycle
{
  public:
    bool claim(process_id owner)
    {
        if (owner == 0 || revoked_)
            return false;
        if (owner_ == 0)
            owner_ = owner;
        return owner_ == owner;
    }

    bool owns(process_id owner) const { return owner != 0 && !revoked_ && owner_ == owner; }
    bool revoked_for(process_id owner) const { return owner != 0 && revoked_ && revoked_owner_ == owner; }

    bool owner_exit(process_id owner)
    {
        if (!owns(owner))
            return false;
        revoked_owner_ = owner;
        owner_ = 0;
        revoked_ = true;
        return true;
    }

    bool revoked() const { return revoked_; }

  private:
    process_id owner_ = 0;
    process_id revoked_owner_ = 0;
    bool revoked_ = false;
};
} // namespace acpi
