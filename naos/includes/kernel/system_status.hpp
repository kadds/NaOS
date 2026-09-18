#pragma once

#include "kernel/kobject.hpp"

namespace system_status_service
{

class object final : public kobject
{
  public:
    object()
        : kobject(type_e::system_status)
    {
    }

    static type_e type_of() { return type_e::system_status; }
};

} // namespace system_status_service
