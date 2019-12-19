#include "kernel/dev/device.hpp"
#include "kernel/dev/driver.hpp"
#include "kernel/mm/new.hpp"
namespace dev
{
device_id_gen_t *id_gen;
device_map_t *device_map;
device_map_t *unbinding_device_map;

void init()
{
    id_gen = memory::New<device_id_gen_t>(memory::KernelCommonAllocatorV, 4096);
    device_map = memory::New<device_map_t>(memory::KernelCommonAllocatorV, memory::KernelCommonAllocatorV);
    unbinding_device_map = memory::New<device_map_t>(memory::KernelCommonAllocatorV, memory::KernelCommonAllocatorV);
    init_driver();
}

int enum_device(device_class *clazz)
{
    int dev_index = 0;
    while (1)
    {
        device *dev = clazz->try_scan(dev_index);
        if (dev != nullptr)
        {
            auto id = id_gen->next();
            if (id == util::null_id)
            {
                trace::panic("Too many device register to system");
            }
            dev->id = id;
            unbinding_device_map->insert(id, dev);
            dev_index++;
        }
        else
        {
            break;
        }
    }
    return dev_index;
}

} // namespace dev
