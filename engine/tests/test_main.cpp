// Doctest entry point for every module's tests (test_<module>.cpp files live next to this one).
#define DOCTEST_CONFIG_IMPLEMENT_WITH_MAIN
#include <doctest.h>
TEST_CASE("test harness runs") { CHECK(1 + 1 == 2); }
