#include "kernel/service_directory.hpp"

#include "kernel/errno.hpp"
#include "kernel/mm/memory.hpp"
#include "kernel/ucontext.hpp"

namespace service
{

directory::directory()
    : kobject(type_e::service_directory)
    , entries_(memory::KernelCommonAllocatorV)
{
}

directory::~directory()
{
    uctx::RawSpinLockUninterruptibleContext guard(lock_);
    for (auto &value : entries_)
        release_entry(value);
    entries_.clear();
}

bool directory::valid_name(const char *name, u64 name_size)
{
    if (name == nullptr || name_size == 0 || name_size > 255)
        return false;
    for (u64 i = 0; i < name_size; i++)
    {
        const auto value = static_cast<unsigned char>(name[i]);
        if (value == 0 || value == '/' || value < 0x20 || value == 0x7f)
            return false;
    }
    return true;
}

i64 directory::find_locked(const char *name, u64 name_size) const
{
    const auto entries = entries_.cspan();
    for (u64 i = 0; i < entries_.size(); i++)
    {
        if (entries[i].name.size() == name_size &&
            memcmp(entries[i].name.data(), name, static_cast<size_t>(name_size)) == 0)
            return static_cast<i64>(i);
    }
    return -1;
}

void directory::release_entry(entry &value)
{
    if (value.object)
        value.object->on_capability_release(capability::location::table_root);
    value.object.reset();
}

i64 directory::register_service(const char *name, u64 name_size, capability::transfer_record &record)
{
    if (!valid_name(name, name_size) || !record.moved || !record.resource.valid())
        return EINVAL;

    uctx::RawSpinLockUninterruptibleContext guard(lock_);
    if (find_locked(name, name_size) >= 0)
        return EEXIST;

    freelibcxx::string stored_name(memory::KernelCommonAllocatorV, name, static_cast<int>(name_size));
    if (stored_name.size() != name_size)
        return ENOMEM;
    auto object = record.resource.take_object_to_table();
    if (!object)
        return EINVAL;
    entries_.push_back(std::move(stored_name), std::move(object), record.resource.meta());
    return 0;
}

i64 directory::resolve_service(const char *name, u64 name_size, capability::transferred_resource &resource)
{
    if (!valid_name(name, name_size))
        return EINVAL;

    uctx::RawSpinLockUninterruptibleContext guard(lock_);
    const auto index = find_locked(name, name_size);
    if (index < 0)
        return ENOENT;

    auto &value = entries_[static_cast<u64>(index)];
    if (!value.object)
        return EIO;
    resource = capability::transferred_resource(value.object, value.metadata);
    if (!value.object->capability_is_unique())
        return 0;

    value.object->on_capability_handoff(capability::location::table_root, capability::location::in_transit);
    entries_.remove_at(static_cast<u64>(index));
    return 0;
}

i64 directory::unregister_service(const char *name, u64 name_size)
{
    if (!valid_name(name, name_size))
        return EINVAL;

    uctx::RawSpinLockUninterruptibleContext guard(lock_);
    const auto index = find_locked(name, name_size);
    if (index < 0)
        return ENOENT;
    auto &value = entries_[static_cast<u64>(index)];
    release_entry(value);
    entries_.remove_at(static_cast<u64>(index));
    return 0;
}

i64 directory::list_services(u64 offset, u64 requested_bytes, freelibcxx::vector<byte> &records, u64 &next,
                             u64 &count) const
{
    if (offset > entries_.size() || requested_bytes > NA_CHANNEL_MAX_MESSAGE_BYTES)
        return EINVAL;

    uctx::RawSpinLockUninterruptibleContext guard(lock_);
    records.clear();
    next = offset;
    count = 0;
    const auto entries = entries_.cspan();
    for (u64 i = offset; i < entries_.size(); i++)
    {
        const auto &name = entries[i].name;
        if (name.size() + 1 > requested_bytes - records.size())
            break;
        for (u64 j = 0; j < name.size(); j++)
            records.push_back(static_cast<byte>(name.data()[j]));
        records.push_back(static_cast<byte>(0));
        next = i + 1;
        count++;
    }
    return 0;
}

} // namespace service
