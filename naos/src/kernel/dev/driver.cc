#include "kernel/dev/driver.hpp"
#include "kernel/dev/device.hpp"
namespace dev
{
driver_id_gen_t *driver_id_gen;
driver_map_t *driver_map;

void init_driver()
{
    driver_id_gen = memory::New<driver_id_gen_t>(memory::KernelCommonAllocatorV, 1, 1);
    driver_map = memory::New<driver_map_t>(memory::KernelCommonAllocatorV, memory::KernelCommonAllocatorV);
}

num_t add_driver(driver *driver)
{
    u64 id = driver_id_gen->next();
    if (id == util::null_id)
        return null_num;

    for (auto it : *unbinding_device_map)
    {
        if (driver->setup(it.value))
        {
            it.value->set_driver(driver);
            driver->id = id;
            unbinding_device_map->remove(it.key);
            device_map->insert(it.key, it.value);
            return it.value->get_dev_id();
        }
    }
    return null_num;
}

void remove_driver(driver *driver) {}

driver *get_driver(num_t dri)
{
    driver *pdri = nullptr;
    driver_map->get(dri, &pdri);
    return pdri;
}
} // namespace dev
