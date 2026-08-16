#pragma once
#include "capability.hpp"
#include "freelibcxx/hash.hpp"
#include "freelibcxx/hash_map.hpp"
#include "freelibcxx/string.hpp"
#include "freelibcxx/vector.hpp"
#include "handle.hpp"
#include "kernel/kobject.hpp"
#include "lock.hpp"
#include "mm/new.hpp"
#include "types.hpp"
#include <atomic>
namespace task
{
class resource_table_t
{
  private:
    using native_handle_map_t = freelibcxx::hash_map<na_handle_t, capability::entry>;
    using native_node_t = native_handle_map_t::node_t;
    native_handle_map_t native_handle_map;
    lock::rw_lock_t native_map_lock;
    na_handle_t next_native_handle;
    u64 native_entry_count;

    na_handle_t allocate_native_handle_locked();
    void clear_native_locked(freelibcxx::vector<khandle> &released);

  public:
    resource_table_t();
    ~resource_table_t();
    na_handle_t install_native(khandle object, capability::metadata meta = {});

    na_status_t reserve_native(freelibcxx::vector<na_handle_t> &handles, u64 count);
    na_status_t activate_native(na_handle_t handle, capability::transferred_resource &&resource);
    void rollback_native(const freelibcxx::vector<na_handle_t> &handles);

    bool lookup_native(na_handle_t handle, capability::entry &entry);
    bool has_native_object_type(kobject::type_e type);
    bool has_native_protocol_right(u64 right);
    na_signal_t native_signals(na_handle_t handle);
    na_status_t close_native(na_handle_t handle);
    na_status_t duplicate_native(na_handle_t source, na_meta_rights_t requested_rights, na_handle_t &result);
    // Restrict is a prepare step: source becomes internally pending and the
    // returned slot stays RESERVED until commit_restrict publishes it.
    na_status_t restrict_native(na_handle_t source, const na_handle_restriction_t &restriction, na_handle_t &result,
                                capability::entry &source_backup);
    na_status_t commit_restrict(na_handle_t source, na_handle_t restricted);
    na_status_t rollback_restrict(na_handle_t source, na_handle_t restricted, capability::entry &source_backup);
    // Fork compatibility copies explicitly duplicable bindings and the
    // terminal client-end exception needed for post-fork endpoint rebinding.
    // Other unique or non-duplicable capabilities are intentionally omitted.
    na_status_t clone_fork_bindings(const resource_table_t &source);

    na_status_t take_native_batch(const na_resource_disposition_t *dispositions, u64 count, na_handle_t target,
                                  capability::transfer_record_list &records);
    [[nodiscard]] na_status_t restore_native_batch(capability::transfer_record_list &records);
    void commit_native_batch(capability::transfer_record_list &records);

    void clear();

  private:
    static void discard_transfer_node(void *context, void *slot);
};
} // namespace task
