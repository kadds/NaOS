#include "catch2_compat.hpp"
#include "consoled/key_input.hpp"

#include <string_view>

#include <vterm.h>

TEST_CASE("consoled maps the physical delete key to the terminal delete key")
{
    VTermKey key = VTERM_KEY_NONE;
    REQUIRE(consoled::key_to_vterm(consoled::console_key::delete_key, key));
    REQUIRE(key == VTERM_KEY_DEL);
}

TEST_CASE("consoled delete emits the sequence expected by BusyBox line editing")
{
    VTerm *terminal = vterm_new(1, 80);
    REQUIRE(terminal != nullptr);

    vterm_keyboard_key(terminal, VTERM_KEY_DEL, VTERM_MOD_NONE);
    char output[16]{};
    const auto size = vterm_output_read(terminal, output, sizeof(output));
    REQUIRE(size == 4);
    REQUIRE(std::string_view(output, size) == "\x1b[3~");

    vterm_free(terminal);
}
