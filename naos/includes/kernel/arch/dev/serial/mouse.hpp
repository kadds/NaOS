#pragma once
#include "../../../dev/device.hpp"
#include "../../../dev/driver.hpp"

namespace arch::device::serial::mouse
{
struct device_class : public ::dev::device_class
{
    ::dev::device *try_scan(int index) override {}
};

class device : public ::dev::device
{
};

class driver : public ::dev::char_driver
{
};

} // namespace arch::device::serial::mouse
