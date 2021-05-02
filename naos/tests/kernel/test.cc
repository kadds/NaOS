#include <chrono>
#include <iomanip>
#include <iostream>
#include <regex>
#include <sstream>
#include <string>
#include <unordered_map>
#include <unordered_set>

std::unordered_map<std::string, std::unordered_map<std::string, std::function<void()>>> *map;
typedef std::chrono::high_resolution_clock clockx;

void print_help()
{
    std::cout <<
        R"(    -f <regex> : filter tests. regex.
    -l : list all tests.
    -h : show this help.
    -t <number> : test times for each of tests. default 1.
)" << std::flush;
}

void parse_args(int argc, char *argv[], std::unordered_map<std::string, std::string> &arg_map)
{
    bool is_key = true;
    std::string last;
    for (int i = 1; i <= argc; i++)
    {
        std::string v = i < argc ? argv[i] : "";
        if (is_key)
        {
            if (v == "-l" || v == "-h")
            {
                arg_map[v] = "Y";
                continue;
            }
            is_key = false;
            last = std::move(v);
        }
        else
        {
            arg_map[last] = v;
            last = "";
            is_key = true;
        }
    }
}

int main(int argc, char *argv[])
{
    std::unordered_map<std::string, std::string> args;
    parse_args(argc, argv, args);

    std::regex regex(".*");
    bool only_list = false;
    int times = 1;

    if (args.count("-h") > 0)
    {
        print_help();
        return 0;
    }
    if (args.count("-l") > 0)
    {
        only_list = true;
    }
    if (args.count("-f"))
    {
        std::string s = args["-f"];
        regex = std::regex(s);
    }
    if (args.count("-t"))
    {
        times = strtoll(args["-t"].c_str(), 0, 10);
    }

    for (auto m : *map)
    {
        for (auto i : m.second)
        {
            std::string test_name = m.first + "." + i.first;
            if (std::regex_match(test_name, regex))
            {
                if (!only_list)
                {
                    std::cout << test_name << " ";
                    clockx::time_point t1;
                    clockx::time_point t2;
                    t1 = clockx::now();
                    for (int j = 0; j < times; j++)
                    {
                        i.second();
                    }
                    t2 = clockx::now();
                    auto dt = t2 - t1;
                    if (dt.count() > 1'000'0000)
                    {
                            std::cout << "\r" << std::setw(20) << test_name << std::setw(10)
                                      << std::chrono::duration_cast<std::chrono::milliseconds>(dt).count() << "ms"
                                      << std::endl;
                    }
                    else
                    {
                            std::cout << "\r" << std::setw(20) << test_name << std::setw(10)
                                      << std::chrono::duration_cast<std::chrono::microseconds>(dt).count() << "us"
                                      << std::endl;
                    }                
                }
                else
                {
                    std::cout << test_name << std::endl;
                }
            }
        }
    }
    if (!only_list)
    {
        std::cout << "All tests tests done." << std::endl;
    }
    return 0;
}

int add_test(std::function<void()> f, std::string module, std::string name)
{
    if (!map)
    {
        map = new std::unordered_map<std::string, std::unordered_map<std::string, std::function<void()>>>();
    }
    auto &m = *map;
    m[module][name] = f;
    return map->size();
}