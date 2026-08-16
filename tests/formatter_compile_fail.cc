#include "freelibcxx/formatter.hpp"

int main()
{
    char buffer[16]{};
    (void)freelibcxx::format_to(freelibcxx::span<char>(buffer, sizeof(buffer)), "{:d}", "not an integer");
}
