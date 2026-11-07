#pragma once

#include "kernel/capability.hpp"
#include "kernel/ipc/invocation.hpp"
#include "kernel/mm/memory.hpp"
#include "kernel/types.hpp"
#include "naos/abi.h"
#include <utility>

namespace naos::ipc::kernel_dispatch
{

inline bool descriptor_allows_method(const na_protocol_descriptor_t &descriptor, u64 method_id) noexcept
{
    if (method_id == 0 || method_id > descriptor.method_count || method_id > NA_PROTOCOL_MAX_METHOD_ID)
        return false;
    const u64 word = (method_id - 1) / 64;
    const u64 bit = (method_id - 1) % 64;
    return (descriptor.method_bitmap[word] & (1ULL << bit)) != 0;
}

inline bool descriptor_allows_oneway(const na_protocol_descriptor_t &descriptor, u64 method_id) noexcept
{
    if (!descriptor_allows_method(descriptor, method_id))
        return false;
    const u64 word = (method_id - 1) / 64;
    const u64 bit = (method_id - 1) % 64;
    return (descriptor.oneway_bitmap[word] & (1ULL << bit)) != 0;
}

inline bool descriptor_allows_method_rights(const na_protocol_descriptor_t &descriptor, u64 method_id,
                                            u64 protocol_rights) noexcept
{
    return descriptor_allows_method(descriptor, method_id) &&
           (protocol_rights & descriptor.method_rights[method_id - 1]) == descriptor.method_rights[method_id - 1];
}

enum class method_result_kind : u8
{
    reply,
    protocol_error,
    execution_failure,
    interrupted,
};

using completion_callback = void (*)(void *context, bool committed);

struct method_result
{
    method_result_kind kind = method_result_kind::reply;
    u8 execution_outcome = NA_EXECUTION_NONE;
    na_outcome_reason_t outcome_reason = NA_OUTCOME_REASON_NONE;
    i64 protocol_error = 0;
    completion_callback completion = nullptr;
    void *completion_context = nullptr;

    static method_result success() { return {}; }

    static method_result interrupted()
    {
        method_result result{};
        result.kind = method_result_kind::interrupted;
        return result;
    }

    static method_result success_with_completion(void *context, completion_callback callback)
    {
        method_result result{};
        result.completion = callback;
        result.completion_context = context;
        return result;
    }

    static method_result error(i64 value)
    {
        method_result result{};
        result.kind = method_result_kind::protocol_error;
        result.protocol_error = value;
        return result;
    }

    static method_result error_reply(i64 value)
    {
        method_result result{};
        result.protocol_error = value;
        return result;
    }

    static method_result failure(na_execution_outcome_t outcome, na_outcome_reason_t reason, i64 error = 0)
    {
        method_result result{};
        result.kind = method_result_kind::execution_failure;
        result.protocol_error = error;
        result.execution_outcome = static_cast<u8>(outcome);
        result.outcome_reason = reason;
        return result;
    }
};

static_assert(sizeof(method_result) == 32);

namespace detail
{
template <typename ResourceList>
bool validate_transfer_metadata(void *context, na_handle_t index, u32 expected_binding, u64 expected_scope,
                                na_meta_rights_t expected_meta_rights, u64 expected_protocol_rights)
{
    const auto *resources = static_cast<const ResourceList *>(context);
    if (resources == nullptr || index >= resources->size())
        return false;

    const auto &resource = resources->at(index).resource;
    if (!resource.valid())
        return false;
    const auto &metadata = resource.meta();
    return (expected_binding == NA_BINDING_NONE || metadata.binding == expected_binding) &&
           (expected_scope == 0 || metadata.scope == expected_scope) &&
           (metadata.meta_rights & expected_meta_rights) == expected_meta_rights &&
           (metadata.protocol_rights & expected_protocol_rights) == expected_protocol_rights;
}

inline void ignore_transfer_close(void *, na_handle_t) {}
} // namespace detail

/// Validate generated resource indexes, metadata and transfer dispositions
/// against kernel-owned transfer records without converting them into user
/// handle-table entries.
template <typename Message, typename ResourceList, typename ResourceValidator, typename MetadataValidator,
          typename DispositionValidator>
bool validate_transfer_contract(const Message &message, const ResourceList &resources,
                                ResourceValidator validate_resources, MetadataValidator validate_metadata,
                                DispositionValidator validate_dispositions)
{
    const u64 count = resources.size();
    if (count > NA_CHANNEL_MAX_RESOURCES || !validate_resources(message, count))
        return false;

    freelibcxx::vector<na_handle_t> handle_indexes(memory::KernelCommonAllocatorV);
    freelibcxx::vector<na_resource_disposition_t> dispositions(memory::KernelCommonAllocatorV);
    handle_indexes.ensure(count);
    dispositions.ensure(count);
    if (handle_indexes.capacity() < count || dispositions.capacity() < count)
        return false;

    for (u64 index = 0; index < count; ++index)
    {
        const auto &record = resources.at(index);
        if (!record.resource.valid())
            return false;

        handle_indexes.push_back(static_cast<na_handle_t>(index));
        na_resource_disposition_t disposition{};
        disposition.handle = static_cast<na_handle_t>(index);
        disposition.operation = record.moved ? NA_RESOURCE_MOVE : NA_RESOURCE_DUPLICATE;
        disposition.rights = record.resource.meta().meta_rights;
        disposition.scope = record.resource.meta().scope;
        dispositions.push_back(disposition);
    }

    const auto *handles = count == 0 ? nullptr : handle_indexes.data();
    const auto *wire_dispositions = count == 0 ? nullptr : dispositions.data();
    return validate_metadata(message, handles, count, detail::validate_transfer_metadata<ResourceList>,
                             detail::ignore_transfer_close, const_cast<ResourceList *>(&resources)) &&
           validate_dispositions(message, wire_dispositions, count);
}

template <typename Response, typename Encoder, typename ResourceValidator, typename MetadataValidator,
          typename DispositionValidator>
na_status_t complete_method(invocation_state &state, const method_result &result, const Response &response,
                            capability::transfer_record_list &&resources, Encoder encode,
                            ResourceValidator validate_resources, MetadataValidator validate_metadata,
                            DispositionValidator validate_dispositions)
{
    const auto notify_completion = [&result](bool committed) {
        if (result.completion != nullptr)
            result.completion(result.completion_context, committed);
    };

    switch (result.kind)
    {
        case method_result_kind::protocol_error: {
            notify_completion(false);
            freelibcxx::vector<byte> bytes(memory::MemoryAllocatorV);
            capability::transfer_record_list empty(memory::KernelCommonAllocatorV);
            return state.complete_reply(std::move(bytes), std::move(empty), result.protocol_error)
                       ? NA_STATUS_OK
                       : NA_STATUS_PEER_CLOSED;
        }
        case method_result_kind::execution_failure:
            notify_completion(false);
            return state.complete_failure(static_cast<na_execution_outcome_t>(result.execution_outcome),
                                          result.outcome_reason, result.protocol_error)
                       ? NA_STATUS_OK
                       : NA_STATUS_PEER_CLOSED;
        case method_result_kind::interrupted:
            return NA_STATUS_PEER_CLOSED;
        case method_result_kind::reply:
            break;
    }

    if (!validate_transfer_contract(response, resources, validate_resources, validate_metadata, validate_dispositions))
    {
        notify_completion(false);
        return NA_STATUS_INVALID_MESSAGE;
    }

    freelibcxx::vector<byte> bytes(memory::MemoryAllocatorV);
    u64 written = 0;
    if (!encode(nullptr, 0, response, written) && written == 0)
    {
        notify_completion(false);
        return NA_STATUS_RESOURCE_EXHAUSTED;
    }
    if (written > NA_CHANNEL_MAX_MESSAGE_BYTES)
    {
        notify_completion(false);
        return NA_STATUS_RESOURCE_EXHAUSTED;
    }
    bytes.ensure(written);
    if (bytes.capacity() < written)
    {
        notify_completion(false);
        return NA_STATUS_RESOURCE_EXHAUSTED;
    }
    bytes.resize(written, byte{});
    if (written != 0 && bytes.data() == nullptr)
    {
        notify_completion(false);
        return NA_STATUS_RESOURCE_EXHAUSTED;
    }
    u64 encoded = 0;
    if (!encode(written == 0 ? nullptr : reinterpret_cast<u8 *>(bytes.data()), written, response, encoded) ||
        encoded != written)
    {
        notify_completion(false);
        return NA_STATUS_RESOURCE_EXHAUSTED;
    }
    const bool committed = state.complete_reply(std::move(bytes), std::move(resources), result.protocol_error);
    notify_completion(committed);
    return committed ? NA_STATUS_OK : NA_STATUS_PEER_CLOSED;
}

} // namespace naos::ipc::kernel_dispatch
