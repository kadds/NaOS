#pragma once

#include "kernel/ipc/invocation.hpp"
#include "kernel/ipc/kernel_method_dispatch.hpp"
#include "kernel/types.hpp"

namespace task
{
struct process_t;
}

namespace naos::ipc::kernel_dispatch
{
struct kernel_view_context
{
    capability::entry &target;
    invocation_state &state;
    u64 method_id;
    const freelibcxx::vector<byte> &request;
    capability::transfer_record_list &resources;
    task::process_t *caller;
    thread_id caller_tid;
    u64 caller_io_lease_generation;
};

using kernel_view_handler = na_status_t (*)(kernel_view_context &context);
template <typename Object>
using typed_kernel_view_handler = na_status_t (*)(kernel_view_context &context, Object &object);

struct kernel_view_route
{
    kobject::type_e object_type;
    u64 scope;
    const na_protocol_descriptor_t *descriptor; // IDL method availability and protocol-right policy.
    kernel_view_handler handler;
};

struct kernel_view_route_table
{
    const kernel_view_route *routes;
    u64 count;
};

template <typename Object, typed_kernel_view_handler<Object> Handler>
na_status_t dispatch_typed_kernel_view(kernel_view_context &context)
{
    auto *object = context.target.object->get<Object>();
    if (object == nullptr)
        return NA_STATUS_WRONG_BINDING;
    return Handler(context, *object);
}

template <typename Object, typed_kernel_view_handler<Object> Handler>
constexpr kernel_view_route make_kernel_view_route(u64 scope, const na_protocol_descriptor_t &descriptor) noexcept
{
    return {Object::type_of(), scope, &descriptor, dispatch_typed_kernel_view<Object, Handler>};
}

template <u64 TableCount>
na_status_t dispatch_kernel_view(kernel_view_context &context, const kernel_view_route_table (&tables)[TableCount])
{
    const auto object_type = context.target.object->get_ktype();
    for (const auto &table : tables)
    {
        for (u64 index = 0; index < table.count; ++index)
        {
            const auto &route = table.routes[index];
            if (route.object_type != object_type)
                continue;
            if (context.target.meta.scope != route.scope)
                return NA_STATUS_WRONG_SCOPE;
            if (route.descriptor == nullptr)
                return NA_STATUS_WRONG_BINDING;
            if (!descriptor_allows_method(*route.descriptor, context.method_id))
                return NA_STATUS_NOT_SUPPORTED;
            if (!descriptor_allows_method_rights(*route.descriptor, context.method_id,
                                                 context.target.meta.protocol_rights))
                return NA_STATUS_ACCESS_DENIED;
            return route.handler(context);
        }
    }
    return NA_STATUS_WRONG_BINDING;
}
} // namespace naos::ipc::kernel_dispatch
