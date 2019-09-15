#include "kernel/irq.hpp"
#include "kernel/arch/exception.hpp"
#include "kernel/arch/interrupt.hpp"
#include "kernel/mm/list_node_cache.hpp"
#include "kernel/mm/memory.hpp"
#include "kernel/util/linked_list.hpp"

namespace irq
{

typedef util::linked_list<request_func_data> request_list_t;
typedef list_node_cache<request_list_t> list_node_cache_t;
typedef memory::ListNodeCacheAllocator<list_node_cache_t> list_node_cache_allocator_t;

list_node_cache_allocator_t *list_node_cache_allocator;
list_node_cache_t *list_node_cache;
request_list_t *irq_list[256];

void do_irq(const arch::idt::regs_t *regs, u64 extra_data)
{
    request_list_t *list = irq_list[regs->vector];
    for (auto it = list->begin(); it != list->end(); it = list->next(it))
    {
        it->element.func(regs, extra_data, it->element.user_data);
    }
}

void init()
{
    list_node_cache = memory::New<list_node_cache_t>(memory::KernelCommonAllocatorV, memory::KernelBuddyAllocatorV);
    list_node_cache_allocator =
        memory::New<list_node_cache_allocator_t>(memory::KernelCommonAllocatorV, list_node_cache);

    arch::exception::set_callback(&do_irq);
    arch::interrupt::set_callback(&do_irq);
    for (auto &list : irq_list)
    {
        list = memory::New<request_list_t>(memory::KernelCommonAllocatorV, list_node_cache_allocator);
    }
}

void insert_request_func(u32 vector, request_func func, u64 user_data)
{
    irq_list[vector]->push_back(request_func_data(func, user_data));
}

void remove_request_func(u32 vector, request_func func)
{
    request_list_t *list = irq_list[vector];
    for (auto it = list->begin(); it != list->end(); it = list->next(it))
    {
        if (it->element.func == func)
        {
            list->remove(it);
            return;
        }
    }
}

} // namespace irq