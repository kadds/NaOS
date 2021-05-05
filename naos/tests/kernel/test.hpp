#pragma once
#include "comm.hpp"
#include <functional>
#include <iostream>
#include <string>
#include <thread>

int add_test(std::function<void()> f, std::string module, std::string name);

#define test(module, name) int x##module##_##name = add_test(test_##module##_##name, #module, #name);

#define assert(exp)                                                                                                    \
    do                                                                                                                 \
    {                                                                                                                  \
        if (!(exp))                                                                                                    \
        {                                                                                                              \
            std::cerr << #exp << " failed at " << __FILE__ << ":" << __LINE__ << std::endl;                            \
            exit(-1);                                                                                                  \
        }                                                                                                              \
    } while (0)

struct Int
{
    Int(int i, int s = 0)
        : inner(new int(i))
        , v(i)
        , s(s)
    {
    }

    Int(const Int &rhs)
        : inner(new int(*rhs.inner))
        , v(rhs.v)
        , s(rhs.s)
    {
    }

    Int(Int &&rhs)
        : inner(rhs.inner)
        , v(rhs.v)
        , s(rhs.s)
    {
        rhs.inner = nullptr;
        rhs.v = 0;
        rhs.s = 0;
    }

    Int &operator=(const Int &rhs)
    {
        if (this == &rhs)
        {
            return *this;
        }
        if (inner)
        {
            delete inner;
        }
        inner = new int(*rhs.inner);
        v = rhs.v;
        s = rhs.s;

        return *this;
    }

    Int &operator=(Int &&rhs)
    {
        if (this == &rhs)
        {
            return *this;
        }
        if (inner)
        {
            delete inner;
        }
        inner = rhs.inner;
        v = rhs.v;
        s = rhs.s;
        rhs.inner = nullptr;
        rhs.v = 0;
        rhs.s = 0;
        return *this;
    }

    bool operator==(const Int &rhs) const { return *inner == *rhs.inner && v == rhs.v && s == rhs.s; }

    bool operator!=(const Int &rhs) const { return !operator==(rhs); }

    ~Int()
    {
        if (inner)
        {
            assert(v == *inner);
            delete inner;
        }
    }
    int *inner;
    int v;
    int s;
};