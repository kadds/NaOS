#include "freelibcxx/formatter.hpp"
#include "freelibcxx/iterator.hpp"
#include "freelibcxx/random.hpp"
#include "freelibcxx/string.hpp"
#include "nanobox.hpp"
#include <cstdio>
#include <unistd.h>

int simd_test(int argc, char **argv)
{
    int times = 1000;
    if (argc > 1)
    {
        times = freelibcxx::const_string_view(argv[1]).to_int().value_or(1000);
    }
    freelibcxx::string ss(&alloc);
    freelibcxx::mt19937_random_engine rng(0);
    freelibcxx::random_generator r(rng);

    for (int i = 0; i < 100000; i++)
    {
        char c = 'a' + r.gen_range(0, 26);
        ss.append_buffer(&c, 1);
    }

    int n = 0;
    for (int i = 0; i < times; i++)
    {
        int index = 0;
        while (true)
        {
            auto view = ss.view();
            auto p = freelibcxx::find(view.begin() + index, view.end(), 'b');
            if (p != view.end())
            {
                index = p - view.begin() + 1;
                n++;
            }
            else
            {
                break;
            }
            n += strlen(&view[0]);
        }
    }
    printf("find %d\n", n);

    return 0;
}