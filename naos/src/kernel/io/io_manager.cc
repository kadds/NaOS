#include "kernel/io/io_manager.hpp"
#include "freelibcxx/hash_map.hpp"
#include "freelibcxx/vector.hpp"

namespace io
{
struct hash
{
    u64 operator()(const u32 &id) { return id; }
};

using request_chain_t = freelibcxx::vector<u32>;
using request_map_t = freelibcxx::hash_map<u32, request_chain_t *, hash>;

request_map_t *request_map;

void init()
{
    request_map = memory::New<request_map_t>(memory::KernelCommonAllocatorV, memory::KernelCommonAllocatorV);
}

bool send_io_request(request_t *request)
{
    request_chain_t *chain;
    auto chain_opt = request_map->get(request->type);

    if (chain_opt.has_value())
    {
        chain = chain_opt.value();
        u64 size = chain->size();
        auto stack = memory::NewArray<io_stack_t>(memory::KernelCommonAllocatorV, size);
        request->inner.stack_size = (u32)size;
        request->inner.cur_stack_index = 0;
        request->inner.io_stack = stack;
        for (u64 i = 0; i < size; i++)
        {
            request->inner.io_stack[i].dev_num = (*chain)[i];
            request->inner.io_stack[i].completion_callback = nullptr;
        }

        auto dev = chain->at(0);
        auto device = ::dev::get_device(dev);
        if (unlikely(device == nullptr))
        {
            memory::DeleteArray<io_stack_t>(memory::KernelCommonAllocatorV, stack, size);
            request->inner.io_stack = nullptr;
            return false;
        }

        auto driver = device->get_driver();
        if (unlikely(driver == nullptr))
        {
            memory::DeleteArray<io_stack_t>(memory::KernelCommonAllocatorV, stack, size);
            request->inner.io_stack = nullptr;

            return false;
        }
        driver->on_io_request(request);
        if (request->poll)
        {
            memory::DeleteArray<io_stack_t>(memory::KernelCommonAllocatorV, stack, size);
            request->inner.io_stack = nullptr;
        }
        else if (request->status.io_is_completion)
        {
            memory::DeleteArray<io_stack_t>(memory::KernelCommonAllocatorV, stack, size);
            request->inner.io_stack = nullptr;
        }
        return true;
    }
    return false;
}

void finish_io_request(request_t *request)
{
    auto chain_opt = request_map->get(request->type);

    if (chain_opt.has_value())
    {
        if (request->inner.io_stack != nullptr)
            memory::DeleteArray<io_stack_t>(memory::KernelCommonAllocatorV, request->inner.io_stack,
                                            request->inner.stack_size);
    }
}

void call_next_io_request_chain(request_t *request)
{
    auto dev = request->inner.io_stack[++request->inner.cur_stack_index].dev_num;
    auto device = ::dev::get_device(dev);
    kassert(unlikely(device != nullptr), "Device is deleted when request.");
    auto driver = device->get_driver();
    kassert(unlikely(driver != nullptr), "Device (driver) is deleted when request.");

    driver->on_io_request(request);
}

bool attach_request_chain_device(u32 source_dev, u32 target_dev, request_chain_number_t request_chain)
{
    auto chain_opt = request_map->get(request_chain);
    if (chain_opt.has_value())
    {
        auto chain = chain_opt.value();
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
        auto chain = memory::New<request_chain_t>(memory::KernelCommonAllocatorV, memory::KernelCommonAllocatorV);
        chain->push_back(source_dev);
        request_map->insert(request_chain, chain);
        return true;
    }
    return false;
}

void remove_request_chain_device(u32 device) {}

void completion(request_t *request)
{
    auto &inner_data = request->inner;
    inner_data.cur_stack_index = inner_data.stack_size - 1;
    bool inter = false;
    while (inner_data.cur_stack_index-- != 0)
    {
        auto &io_stack = inner_data.io_stack;
        auto cur_io = io_stack[inner_data.cur_stack_index];
        if (cur_io.completion_callback != 0)
        {
            if (cur_io.completion_callback(request, cur_io.completion_user_data) == completion_result_t::inter)
            {
                inter = true;
                break;
            }
        }
    }

    if (request->final_completion_func != nullptr)
    {
        request->final_completion_func(request, inter, request->completion_user_data);
    }
}

} // namespace io
