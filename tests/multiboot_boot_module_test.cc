#include <catch2/catch_test_macros.hpp>

#include "kernel/arch/multiboot/boot_modules.hpp"

#include <cstring>

TEST_CASE("Multiboot keeps anonymous module ranges without granting a token", "[multiboot]")
{
    boot_module modules[max_boot_modules];
    u64 count = 0;
    constexpr char anonymous_command_line[] = "";

    REQUIRE(arch::multiboot::append_boot_module(modules, max_boot_modules, count, 0x100000, 0x102000,
                                                anonymous_command_line, sizeof(anonymous_command_line)));
    CHECK(count == 1);
    CHECK(modules[0].name[0] == '\0');
    CHECK(modules[0].start == 0x100000);
    CHECK(modules[0].size == 0x2000);

    constexpr char service_command_line[] = "platformd";
    REQUIRE(arch::multiboot::append_boot_module(modules, max_boot_modules, count, 0x200000, 0x201000,
                                                service_command_line, sizeof(service_command_line)));
    CHECK(count == 2);
    CHECK(std::strcmp(modules[1].name, "platformd") == 0);
}

TEST_CASE("Multiboot rejects malformed module tags and capacity overflow", "[multiboot]")
{
    boot_module modules[max_boot_modules];
    u64 count = 0;
    constexpr char unterminated_command_line[] = {'x', 'y'};
    CHECK_FALSE(arch::multiboot::append_boot_module(modules, max_boot_modules, count, 0x1000, 0x2000,
                                                    unterminated_command_line, sizeof(unterminated_command_line)));
    CHECK_FALSE(arch::multiboot::append_boot_module(modules, max_boot_modules, count, 0x2000, 0x1000, "", 1));
    CHECK_FALSE(arch::multiboot::append_boot_module(modules, 0, count, 0x1000, 0x2000, "", 1));
    CHECK(count == 0);
}
