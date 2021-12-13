#include "catch2/catch.hpp"

#include "globals.hpp"


using namespace m;


TEST_CASE("Options", "[core][globals]")
{
    Options options = Options::Get();

    REQUIRE_FALSE(options.quiet);
    options.quiet = true;
    REQUIRE(options.quiet);
}
