#include "kernel/io/io_manager.hpp"
#include "kernel/util/array.hpp"
#include "kernel/util/hash_map.hpp"

namespace io
{
struct hash
{
    u64 operator()(const u32 &id) { return id; }
};

using request_chain_t = util::array<u32>;
using request_map_t = util::hash_map<u32, request_chain_t *, hash>;

request_map_t *request_map;

void init()
{
    request_map = memory::New<request_map_t>(memory::KernelCommonAllocatorV, memory::KernelCommonAllocatorV);
}

bool send_io_request(request_t *request)
{
    request_chain_t *chain;

    if (request_map->get(request->type, &chain))
    {
        u64 size = chain->size();
        request->dev_stack = memory::NewArray<u32>(memory::KernelCommonAllocatorV, size);
        util::memcopy(request->dev_stack, chain->data(), size * sizeof(dev::num_t));
        auto dev = request->dev_stack[request->cur_stack_index];
        auto device = ::dev::get_device(dev);
        if (unlikely(device == nullptr))
        {
            memory::DeleteArray<u32>(memory::KernelCommonAllocatorV, request->dev_stack, size);
            return false;
        }

        auto driver = device->get_driver();
        if (unlikely(driver == nullptr))
        {
            memory::DeleteArray<u32>(memory::KernelCommonAllocatorV, request->dev_stack, size);
            return false;
        }
        driver->on_io_request(request);
        memory::DeleteArray<u32>(memory::KernelCommonAllocatorV, request->dev_stack, size);
        return true;
    }
    return false;
}

void call_next_io_request_chain(request_t *request)
{
    auto dev = request->dev_stack[++request->cur_stack_index];
    auto device = ::dev::get_device(dev);
    kassert(unlikely(device != nullptr), "Device is deleted when request.");
    auto driver = device->get_driver();
    kassert(unlikely(driver != nullptr), "Device (driver) is deleted when request.");

    driver->on_io_request(request);
}

bool attach_request_chain_device(u32 source_dev, u32 target_dev, request_chain_number_t request_chain)
{
    request_chain_t *chain;

    if (request_map->get(request_chain, &chain))
    {
        if (target_dev == 0)
        {
            chain->push_back(source_dev);
            return true;
        }
        for (auto it = chain->begin(); it != chain->end(); ++it)
        {
            if (*it == target_dev)
            {
                chain->insert(it, source_dev);
                return true;
            }
        }
    }
    else
    {
        chain = memory::New<request_chain_t>(memory::KernelCommonAllocatorV, memory::KernelCommonAllocatorV);
        request_map->insert(request_chain, chain);
        chain->push_back(source_dev);
        return true;
    }
    return false;
}

void remove_request_chain_device(u32 device) {}

} // namespace io
