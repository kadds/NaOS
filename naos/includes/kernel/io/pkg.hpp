#pragma once
#include "common.hpp"
namespace io
{

struct request_t
{
    u16 type;
    u32 dev_id;
};

struct package_t
{
    u32 dev_id;
    u16 type;
};

struct mouse_package_t : package_t
{
    u16 movement;
    struct
    {
        u8 index : 4;
    };
};

struct keyboard_package_t : package_t
{
    u16 key;
};

} // namespace io
