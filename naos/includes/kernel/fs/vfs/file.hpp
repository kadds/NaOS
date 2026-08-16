#pragma once
#include "defines.hpp"
#include "kernel/common.hpp"
#include "kernel/kobject.hpp"
#include "kernel/lock.hpp"
#include "kernel/mutex.hpp"
#include "freelibcxx/function_ref.hpp"
#include "pseudo.hpp"
namespace fs::vfs
{
class dentry;
class file : public kobject
{
  protected:
    i64 offset;
    flag_t mode;
    dentry *entry;
    bool open_reference;
    // A regular read/write can allocate or acquire filesystem locks.  This
    // protects the open-description offset without disabling interrupts for
    // the duration of that work.
    mutable lock::mutex_t io_lock_;

  public:
    file()
        : kobject(type_e::file)
        , offset(0)
        , mode(0)
        , entry(nullptr)
        , open_reference(false)
    {
    }

    file(const file &f) = delete;
    file &operator=(const file &f) = delete;

    virtual ~file();

    virtual int open(dentry *entry, flag_t mode);
    virtual int open(dentry *entry, flag_t mode, const task::access_context &context);

    i64 read(byte *ptr, u64 max_size, flag_t flags, pseudo_t::interruption_check interrupted = nullptr,
             pseudo_t::wait_queue_registration register_wait_queue = nullptr);
    i64 write(const byte *ptr, u64 size, flag_t flags, pseudo_t::interruption_check interrupted = nullptr,
              pseudo_t::wait_queue_registration register_wait_queue = nullptr);

    i64 pread(i64 offset, byte *ptr, u64 max_size, flag_t flags, pseudo_t::interruption_check interrupted = nullptr,
              pseudo_t::wait_queue_registration register_wait_queue = nullptr);
    i64 pwrite(i64 offset, const byte *ptr, u64 size, flag_t flags,
               pseudo_t::interruption_check interrupted = nullptr,
               pseudo_t::wait_queue_registration register_wait_queue = nullptr);
    virtual void flush() = 0;

    int native_sync();
    bool native_truncate(u64 length);
    bool native_allocate(u64 offset, u64 length);
    flag_t native_get_flags() const { return mode; }
    bool native_set_flags(flag_t flags);

    virtual void seek(i64 offset);
    virtual void move(i64 where);
    virtual i64 current_offset();

    u64 size() const;
    dentry *get_entry() const;

    pseudo_t *get_pseudo();
    const pseudo_t *get_pseudo() const;
    flag_t get_mode() const;

    na_signal_t capability_signals() const override;

    static type_e type_of() { return type_e::file; }

  protected:
    virtual i64 iread(i64 &offset, byte *ptr, u64 max_size, flag_t flags) = 0;
    virtual i64 iwrite(i64 &offset, const byte *ptr, u64 size, flag_t flags) = 0;
};
} // namespace fs::vfs
