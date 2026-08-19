#include "naos/abi.h"

#include "catch2_compat.hpp"
#include <cstddef>

namespace
{
void check_public_tcb_prefix_layout()
{
    static_assert(NAOS_TLS_ABI_VERSION == 1U);
    static_assert(sizeof(naos_tls_abi_v1_t) == 0x38);
    static_assert(alignof(naos_tls_abi_v1_t) == alignof(void *));
    static_assert(offsetof(naos_tls_abi_v1_t, self_pointer) == 0x00);
    static_assert(offsetof(naos_tls_abi_v1_t, dtv_size) == 0x08);
    static_assert(offsetof(naos_tls_abi_v1_t, dtv_pointer) == 0x10);
    static_assert(offsetof(naos_tls_abi_v1_t, tid) == 0x18);
    static_assert(offsetof(naos_tls_abi_v1_t, did_exit) == 0x1c);
    static_assert(offsetof(naos_tls_abi_v1_t, stack_canary) == 0x28);
    static_assert(offsetof(naos_tls_abi_v1_t, cancel_bits) == 0x30);
    static_assert(NAOS_TLS_MAX_SIZE == (1ULL << 20));
    static_assert(NAOS_TLS_MAX_ALIGN == (1ULL << 20));
}
} // namespace

TEST_CASE("naos TLS ABI v1 public TCB prefix", "[abi][tls]")
{
    check_public_tcb_prefix_layout();
    REQUIRE(sizeof(naos_tls_abi_v1_t) == 0x38);
}
