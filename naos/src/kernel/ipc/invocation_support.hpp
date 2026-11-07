#pragma once

#include "kernel/ipc/invocation.hpp"
#include "kernel/mm/memory.hpp"
#include "naos/generated/system_uapi.h"
#include <atomic>

namespace naos::ipc::dispatch_detail
{
inline constexpr u64 max_kernel_payload = NA_CHANNEL_MAX_MESSAGE_BYTES;
extern std::atomic_uint64_t status_sample_sequence;

inline freelibcxx::vector<byte> empty_bytes() { return freelibcxx::vector<byte>(memory::MemoryAllocatorV); }

inline capability::transfer_record_list empty_resources()
{
    return capability::transfer_record_list(memory::KernelCommonAllocatorV);
}

template <typename Message, typename Encoder>
bool encode_message(freelibcxx::vector<byte> &destination, const Message &message, Encoder encoder)
{
    u64 written = 0;
    if (!encoder(nullptr, 0, message, written) && written == 0)
        return false;
    destination.resize(written, byte{});
    if (written != 0 && destination.data() == nullptr)
        return false;
    u64 encoded = 0;
    if (!encoder(reinterpret_cast<u8 *>(destination.data()), destination.size(), message, encoded) ||
        encoded != written)
    {
        destination.clear();
        return false;
    }
    return true;
}

template <typename Message, typename Decoder>
bool decode_message(const freelibcxx::vector<byte> &source, Message &message, Decoder decoder)
{
    return decoder(reinterpret_cast<const u8 *>(source.data()), source.size(), message);
}

inline capability::metadata kernel_view_metadata(u64 scope, const na_uuid_t &uuid, na_meta_rights_t rights,
                                                 u64 protocol_rights, u64 revision = 1, u64 features = 0)
{
    capability::metadata metadata;
    metadata.binding = NA_BINDING_KERNEL_VIEW;
    metadata.protocol_uuid = uuid;
    metadata.scope = scope;
    metadata.revision = revision;
    metadata.features = features;
    metadata.meta_rights = rights;
    metadata.protocol_rights = protocol_rights | NA_PROTOCOL_RIGHT_INVOKE;
    return metadata;
}

} // namespace naos::ipc::dispatch_detail
