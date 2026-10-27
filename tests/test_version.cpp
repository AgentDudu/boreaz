#define DOCTEST_CONFIG_IMPLEMENT_WITH_MAIN
#include <doctest/doctest.h>

#include <cstdio>
#include <string>

#include "boreaz/version.h"

TEST_CASE("version string agrees with the numeric parts") {
    char expected[32];
    std::snprintf(expected, sizeof(expected), "%d.%d.%d", boreaz::kVersionMajor,
                  boreaz::kVersionMinor, boreaz::kVersionPatch);
    CHECK(std::string(expected) == boreaz::kVersionString);
}

TEST_CASE("version is a sane pre-release triple") {
    CHECK(boreaz::kVersionMajor >= 0);
    CHECK(boreaz::kVersionMajor + boreaz::kVersionMinor + boreaz::kVersionPatch > 0);
}
