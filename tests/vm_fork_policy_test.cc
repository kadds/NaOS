#include <catch2/catch_test_macros.hpp>

#include "kernel/mm/fork_mapping_policy.hpp"

TEST_CASE("fork rejects direct physical mappings but retains ordinary mappings", "[vm_fork_policy]")
{
    CHECK(memory::vm::fork_policy::can_inherit_mapping(false));
    CHECK_FALSE(memory::vm::fork_policy::can_inherit_mapping(true));
}
