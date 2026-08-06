#pragma once

#include "freelibcxx/string.hpp"
#include "freelibcxx/vector.hpp"
#include "kernel/capability.hpp"
#include "kernel/kobject.hpp"
#include "kernel/lock.hpp"
#include "kernel/types.hpp"

namespace service
{

class directory final : public kobject
{
  public:
    directory();
    ~directory() override;

    static type_e type_of() { return type_e::service_directory; }

    bool capability_is_unique() const override { return false; }
    na_signal_t capability_signals() const override { return NA_SIGNAL_WRITABLE; }

    i64 register_service(const char *name, u64 name_size, capability::transfer_record &record);
    i64 resolve_service(const char *name, u64 name_size, capability::transferred_resource &resource);
    i64 unregister_service(const char *name, u64 name_size);
    i64 list_services(u64 offset, u64 requested_bytes, freelibcxx::vector<byte> &records, u64 &next, u64 &count) const;

  private:
    struct entry
    {
        freelibcxx::string name;
        khandle object;
        capability::metadata metadata;

        entry(freelibcxx::string &&name, khandle &&object, capability::metadata metadata)
            : name(std::move(name))
            , object(std::move(object))
            , metadata(metadata)
        {
        }
    };

    static bool valid_name(const char *name, u64 name_size);
    i64 find_locked(const char *name, u64 name_size) const;
    void release_entry(entry &value);

    mutable lock::spinlock_t lock_;
    freelibcxx::vector<entry> entries_;
};

} // namespace service
