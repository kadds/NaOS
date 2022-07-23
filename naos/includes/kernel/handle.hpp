#pragma once
#include "mm/new.hpp"
#include <atomic>
#include <type_traits>
#include <utility>

class raw_handle_t;
struct handle_control
{
  public:
    std::atomic_int64_t ref = 0;
    char t[0];
    template <typename T> T *get() { return reinterpret_cast<T *>(t); }
    template <typename T> const T *get() const { return reinterpret_cast<const T *>(t); }
};

template <typename T> class handle_t
{
  public:
    template <typename... Args> static handle_t make(Args &&...args)
    {
        auto control =
            new (memory::KernelCommonAllocatorV->allocate(sizeof(handle_control) + sizeof(T), alignof(handle_control)))
                handle_control();
        new (control->t) T(std::forward<Args>(args)...);
        return handle_t(control);
    }

    handle_t(handle_control *control = nullptr)
        : control(control)
    {
        if (control != nullptr)
        {
            control->ref++;
        }
    }

    handle_t(const handle_t &rhs) { copy(rhs.get_control()); }

    template <typename U>
    requires std::is_base_of_v<T, U> handle_t(const handle_t<U> &rhs) { copy(rhs.get_control()); }

    handle_t(handle_t &&rhs)
    {
        this->control = rhs.get_control();
        rhs.clear_control();
    }

    template <typename U>
    requires std::is_base_of_v<T, U> handle_t(handle_t<U> &&rhs)
    {
        this->control = rhs.get_control();
        rhs.clear_control();
    }

    handle_t &operator=(const handle_t &rhs)
    {
        auto c = rhs.get_control();
        if (unlikely(this->control == c))
        {
            return *this;
        }
        drop();
        copy(c);
        return *this;
    }

    template <typename U>
    requires std::is_base_of_v<T, U> handle_t &operator=(const handle_t<U> &rhs)
    {
        auto c = rhs.get_control();
        if (unlikely(this->control == c))
        {
            return *this;
        }
        drop();
        copy(c);
        return *this;
    }

    handle_t &operator=(handle_t &&rhs)
    {
        if (unlikely(this->control == rhs.get_control()))
        {
            return *this;
        }
        drop();

        this->control = rhs.get_control();
        rhs.clear_control();
        return *this;
    }

    template <typename U>
    requires std::is_base_of_v<T, U> handle_t &operator=(handle_t<U> &&rhs)
    {
        if (unlikely(this->control == rhs.get_control()))
        {
            return *this;
        }
        drop();

        this->control = rhs.get_control();
        rhs.clear_control();
        return *this;
    }

    explicit operator bool() const { return control != nullptr; }

    ~handle_t() { drop(); }

    T *operator->()
    {
        if (control == nullptr)
        {
            return nullptr;
        }
        return control->get<T>();
    }
    const T *operator->() const
    {
        if (control == nullptr)
        {
            return nullptr;
        }
        return control->get<T>();
    }
    T *operator&()
    {
        if (control == nullptr)
        {
            return nullptr;
        }
        return control->get<T>();
    }
    const T *operator&() const
    {
        if (control == nullptr)
        {
            return nullptr;
        }
        return control->get<T>();
    }

    T &operator*() { return *operator&(); }
    const T &operator*() const { return *operator&(); }

    raw_handle_t as_raw();

    template <typename U> handle_t<U> as() { return handle_t<U>(this->control); }

    handle_control *get_control() const { return control; }
    void clear_control() { control = nullptr; }

    int get_ref_count() const
    {
        if (likely(control != nullptr))
        {
            return control->ref;
        }
        return 0;
    }

    void reset() { drop(); }

  private:
    handle_control *control;

    void drop()
    {
        if (likely(control))
        {
            if (unlikely(--control->ref == 0))
            {
                operator&()->~T();
                memory::Delete<>(memory::KernelCommonAllocatorV, control);
            }
            control = nullptr;
        }
    }

    void copy(handle_control *rhs)
    {
        if (unlikely(rhs == nullptr))
        {
            this->control = rhs;
            return;
        }
        rhs->ref++;
        this->control = rhs;
    }
};

class raw_handle_t
{
  private:
    struct raw_handle_control
    {
        std::atomic_int64_t ref;
    };

  public:
    explicit raw_handle_t(void *control)
        : control(reinterpret_cast<raw_handle_control *>(control))
    {
    }

    raw_handle_t()
        : control(nullptr)
    {
    }

    ~raw_handle_t();

    template <typename T> handle_t<T> as()
    {
        auto c = handle_t<T>(control);
        this->control = nullptr;
        return c;
    }

  private:
    raw_handle_control *control;
};

template <typename T> raw_handle_t handle_t<T>::as_raw()
{
    control->ref.fetch_add(1);
    return raw_handle_t(control);
}
