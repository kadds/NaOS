#include "catch2_compat.hpp"
#include "consoled/render_batch.hpp"

TEST_CASE("consoled delays a framebuffer commit until the output burst settles")
{
    consoled::render_batch batch;

    batch.request(100);
    REQUIRE_FALSE(batch.ready(101));
    REQUIRE(batch.ready(102));

    batch.consume();
    REQUIRE_FALSE(batch.ready(102));
}

TEST_CASE("consoled extends the same frame window for another output chunk")
{
    consoled::render_batch batch;

    batch.request(100);
    batch.request(101);
    REQUIRE_FALSE(batch.ready(102));
    REQUIRE(batch.ready(103));
}
