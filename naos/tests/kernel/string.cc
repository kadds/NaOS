#include "kernel/util/str.hpp"
#include "test.hpp"

using namespace util;

void test_string_create()
{
    string ss(&LibAllocatorV);
    assert(ss.size() == 0);
}
test(string, create);

void test_string_sso()
{
    string ss(&LibAllocatorV, "hi");
    assert(ss.size() == 2 && ss.at(0) == 'h' && ss.at(1) == 'i');
    string ss2("hi");
    assert(ss2.size() == 2 && ss2.at(0) == 'h' && ss2.at(1) == 'i');
}
test(string, sso);

void test_string_append()
{
    string ss(&LibAllocatorV);
    ss += "base1234567890";
    assert(ss.size() == 14);
    ss += "abcdefghi";
    assert(ss.size() == 23);
    ss += "jkl";
    assert(ss.size() == 26 && ss == "base1234567890abcdefghijkl");
    string ss2(&LibAllocatorV, "base");
    ss2 += "123098";
    assert(ss2 == "base123098");
}
test(string, append);

void test_string_copy()
{
    string ss(&LibAllocatorV, "hi");
    string ss2 = ss;
    assert(ss == ss2 && ss2.size() == 2);
    ss2 += "tt";
    string ss3 = ss2;
    assert(ss3 == "hitt" && ss2 == ss3 && ss != ss2);
}
test(string, copy);

void test_string_move()
{
    string ss = string(&LibAllocatorV, "hi");
    string ss2 = std::move(ss);
    assert(ss2 == "hi" && ss.size() == 0);
}
test(string, move);