#pragma once
#include "comm.hpp"
#include <functional>
#include <iostream>
#include <string>

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
